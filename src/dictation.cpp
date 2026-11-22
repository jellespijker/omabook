#include "dictation.h"

#include <QJsonObject>
#include <chrono>
#include <utility>

#include "airegistry.h"
#include "failure.h"
#include "recognitionroute.h"
#include "settings.h"
#include "voxtypelocal.h"

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace {

// Less than this is a press of the key, not something said.
constexpr std::chrono::milliseconds shortest = 300ms;

[[nodiscard]] QString dictationWay(const Settings &settings) { return settings.recognition().value("dictation"_L1).toString(); }

} // namespace

Dictation::Dictation(Settings *settings, AudioDevices *devices, QObject *parent)
    : Dictation(settings, devices, RecognitionQueue::routeFor(*settings), parent) {}

Dictation::Dictation(Settings *settings, AudioDevices *devices, RecognitionQueue::Route route, QObject *parent)
    // Nothing said for a field is kept: the queue keeps no results.
    : QObject(parent), m_settings(settings), m_devices(devices), m_queue(std::move(route), [] { return nullptr; }) {
    m_ticks.setInterval(100ms);
    connect(&m_ticks, &QTimer::timeout, this, &Dictation::progressed);
    connect(m_settings, &Settings::changed, this, &Dictation::settingsChanged);
    // voxtype may say no to stopping too, once the field was handed over.
    connect(&m_voxtype, &VoxtypeDictation::failed, this, [this](const QString &why) {
        if (m_byVoxtype)
            reset();
        fail(m_voxtypeField, why);
    });
}

Dictation::~Dictation() { cancel(); }

QVariantMap Dictation::status(bool hosted, bool typing) const {
    QVariantMap status = AiRegistry::status(*m_settings, Capability::SpeechToText, {.model = {}, .hosted = hosted}).toJson().toVariantMap();
    const bool voxtype = typing && withVoxtype() && voxtypeFound() && !m_settings->aiOff();
    status.insert(u"voxtype"_s, voxtype);
    if (voxtype) {
        // voxtype's own dictation is on this computer whatever writes out recordings.
        status.insert(u"available"_s, true);
        status.insert(u"hosted"_s, false);
    }
    return status;
}

double Dictation::elapsed() const { return m_clip ? static_cast<double>(m_clip->elapsed().count()) : 0.0; }

bool Dictation::voxtypeFound() const { return VoxtypeLocal::missing(RecognitionRoute::voxtypeProgram(*m_settings)).isEmpty(); }

bool Dictation::withVoxtype() const { return dictationWay(*m_settings) == "voxtype"_L1; }

void Dictation::setWithVoxtype(bool on) {
    if (on == withVoxtype())
        return;
    QJsonObject chosen = m_settings->recognition();
    if (on)
        chosen.insert("dictation"_L1, u"voxtype"_s);
    else
        chosen.remove("dictation"_L1);
    m_settings->setRecognition(chosen);
}

int Dictation::listen(const QString &who, bool hosted) { return start(who, hosted, true); }

// Omabook's own, so a hosted way may write it out where it is allowed, as for a note.
int Dictation::listenForCommand() {
    const int field = start({}, true, false);
    if (m_field == field) {
        m_forCommand = true;
        emit changed();
    }
    return field;
}

int Dictation::start(const QString &who, bool hosted, bool typing) {
    const int field = m_next++;
    if (m_field != 0) {
        fail(field, Failure(Failure::Code::DictationBusy).text());
        return field;
    }
    if (m_settings->aiOff()) {
        fail(field, Failure(Failure::Code::AiOff).text());
        return field;
    }
    if (typing && withVoxtype() && voxtypeFound()) {
        m_voxtype.setProgram(RecognitionRoute::voxtypeProgram(*m_settings));
        m_field = field;
        m_voxtypeField = field;
        m_who = who;
        m_byVoxtype = true;
        m_voxtype.start();
        emit changed();
        return field;
    }
    const CapabilityStatus speech = AiRegistry::status(*m_settings, Capability::SpeechToText, {.model = {}, .hosted = hosted});
    if (!speech.available) {
        fail(field, speech.failure.text());
        return field;
    }
    auto clip = DictationClip::start(m_devices->microphone());
    if (!clip) {
        fail(field, clip.error());
        return field;
    }
    m_clip = std::move(*clip);
    // Told after the clip is done telling it, and only while it is the one listening.
    connect(
        m_clip.get(), &DictationClip::ended, this,
        [this, clip = m_clip.get()](const QString &why) {
            if (m_clip.get() == clip)
                ended(why);
        },
        Qt::QueuedConnection);
    m_field = field;
    m_who = who;
    m_hosted = hosted;
    m_ticks.start();
    emit changed();
    return field;
}

void Dictation::stop() {
    if (m_byVoxtype) {
        // voxtype writes it out and types it; the field keeps the keyboard for it.
        const int field = m_field;
        m_voxtype.stop();
        reset();
        emit handedOver(field);
        return;
    }
    if (m_clip && m_clip->listening()) {
        m_clip->stop();
        writeOut();
    }
}

void Dictation::ended(const QString &why) {
    // It reached its longest: what was said so far is written out.
    if (why.isEmpty()) {
        writeOut();
        return;
    }
    const int field = m_field;
    reset();
    fail(field, why);
}

void Dictation::writeOut() {
    m_ticks.stop();
    const int field = m_field;
    if (m_clip->elapsed() < shortest) {
        reset();
        fail(field, Failure(Failure::Code::DictationNothingHeard).text());
        return;
    }
    m_folder = std::make_unique<QTemporaryDir>();
    const QString file = m_folder->filePath(u"dictation.wav"_s);
    if (const auto written = m_clip->write(file); !written) {
        reset();
        fail(field, written.error());
        return;
    }
    m_clip.reset();
    RecognitionQueue::Request request{.input = {.kind = Recognition::Kind::Audio, .strokes = {}, .file = file, .language = {}},
                                      .owner = {},
                                      .hosted = m_hosted,
                                      .again = true};
    m_job = m_queue.submit(std::move(request), [this, field](const std::expected<Recognition::Result, QString> &outcome) {
        if (field != m_field)
            return; // cancelled
        m_job = 0;
        reset();
        const QString text = outcome ? outcome->text.simplified() : QString();
        if (!outcome)
            fail(field, outcome.error());
        else if (text.isEmpty())
            fail(field, Failure(Failure::Code::DictationNothingHeard).text());
        else
            emit heard(field, text);
    });
    emit changed();
}

void Dictation::cancel() {
    if (m_byVoxtype)
        m_voxtype.cancel();
    if (m_job != 0) {
        const int job = m_job;
        m_job = 0;
        m_field = 0; // its answer, given now, is for no field
        m_queue.cancel(job);
    }
    reset();
}

void Dictation::forget(int field) {
    if (field != 0 && field == m_field)
        cancel();
}

void Dictation::reset() {
    const bool was = m_field != 0 || m_clip || m_byVoxtype;
    m_ticks.stop();
    m_clip.reset();
    m_folder.reset();
    m_byVoxtype = false;
    m_forCommand = false;
    m_field = 0;
    m_job = 0;
    m_who.clear();
    m_hosted = false;
    if (was)
        emit changed();
}

void Dictation::fail(int field, const QString &why) {
    // Said a moment later, so the one asking has the field's number first.
    QTimer::singleShot(0, this, [this, field, why] { emit failed(field, why); });
}
