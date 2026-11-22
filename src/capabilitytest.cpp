#include "capabilitytest.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonObject>
#include <QLocale>
#include <QTemporaryDir>
#include <QTimer>
#include <QtConcurrent>
#include <QtMath>
#include <algorithm>
#include <array>
#include <cmath>
#include <expected>
#include <functional>
#include <utility>

#include "ai.h"
#include "aifallback.h"
#include "aiprivacy.h"
#include "airegistry.h"
#include "embeddings.h"
#include "recognition.h"
#include "serverreach.h"
#include "settings.h"
#include "voice.h"
#include "voices.h"

using namespace Qt::StringLiterals;
using Code = Failure::Code;

namespace {

// How a test ends, by the signals of what it asked: what came back, or why
// not as the row's code and the failure's own words.
struct Ends {
    std::function<void(const QString &said)> said;
    std::function<void(const QString &why, const QString &message)> failed;
};

// What a test asks, and how it is stopped before it ends.
struct Worker {
    std::unique_ptr<QObject> object;
    std::function<void()> stop;
};

using Started = std::expected<Worker, Failure>;

// The codes each word of the row stands for; any other is "failed".
struct Worded {
    Code code;
    QLatin1StringView why;
};
constexpr auto worded = std::to_array<Worded>({
    {Code::AiOff, "ai-off"_L1},
    {Code::VoiceAiOff, "ai-off"_L1},
    {Code::AiHostedNotAllowed, "hosted-not-allowed"_L1},
    {Code::AiQuickHostedNotAllowed, "hosted-not-allowed"_L1},
    {Code::HostedNotChosen, "hosted-not-allowed"_L1},
    {Code::MeaningHosted, "hosted-not-allowed"_L1},
    {Code::VoiceServerHosted, "hosted-not-allowed"_L1},
    {Code::AiTimedOut, "timeout"_L1},
    {Code::AiUnreachable, "no-route"_L1},
    {Code::RecognizerUnreachable, "no-route"_L1},
    {Code::RecognizerBusy, "no-route"_L1},
    {Code::NoVisionServer, "no-route"_L1},
    {Code::AiNoCommand, "no-route"_L1},
    {Code::NoVoice, "no-route"_L1},
    {Code::PiperMissing, "no-route"_L1},
    {Code::DispatcherMissing, "no-route"_L1},
    {Code::WhisperMissing, "no-route"_L1},
    {Code::VoxtypeMissing, "no-route"_L1},
    {Code::AiWantsKey, "refused-key"_L1},
    {Code::AiRefusedKey, "refused-key"_L1},
    {Code::AnthropicRefusedKey, "refused-key"_L1},
    {Code::GeminiNoKey, "refused-key"_L1},
    {Code::GeminiRefusedKey, "refused-key"_L1},
    {Code::RecognizerRefusedKey, "refused-key"_L1},
    {Code::VoiceOpenAiNoKey, "refused-key"_L1},
    {Code::VoiceKeyRefused, "refused-key"_L1},
    {Code::AiNoModel, "no-model"_L1},
    {Code::AiNoModelSaid, "no-model"_L1},
    {Code::AiModelRefused, "no-model"_L1},
    {Code::AiListsNoModels, "no-model"_L1},
    {Code::RecognizerNoModel, "no-model"_L1},
    {Code::RecognizerNoVision, "no-model"_L1},
    {Code::RecognizerNoSpeech, "no-model"_L1},
    {Code::WhisperNoModel, "no-model"_L1},
    {Code::PiperNoVoice, "no-model"_L1},
    {Code::VoiceModelRetired, "no-model"_L1},
    {Code::MeaningNoVectors, "no-model"_L1},
});

[[nodiscard]] QVariantMap failedWith(const QString &why, const QString &message) {
    return {{u"state"_s, u"failed"_s}, {u"why"_s, why}, {u"message"_s, message}};
}

// A copy of the settings to test by, theirs staying as they are.
[[nodiscard]] std::unique_ptr<Settings> copyOf(const Settings &settings, const QTemporaryDir &dir) {
    const QString file = dir.filePath(u"config.json"_s);
    if (!dir.isValid() || (QFile::exists(settings.file()) && !QFile::copy(settings.file(), file)))
        return nullptr;
    auto copy = std::make_unique<Settings>(file);
    if (!copy->hasCapabilities())
        AiPrivacy::carryOver(*copy);
    return copy;
}

// A copy of the settings that allows hosted for this capability alone, for
// a test the person said yes to sending to someone else's computer.
[[nodiscard]] std::unique_ptr<Settings> hostedCopy(const Settings &settings, Capability capability, const QTemporaryDir &dir) {
    auto copy = copyOf(settings, dir);
    if (!copy)
        return nullptr;
    QJsonObject all = copy->capabilities();
    QJsonObject one = all.value(Capabilities::name(capability)).toObject();
    one.insert(u"hosted"_s, true);
    all.insert(Capabilities::name(capability), one);
    copy->setCapabilities(all);
    return copy;
}

[[nodiscard]] Started askAnswers(const Settings &settings, Capability capability, const QString &defaultsFile, const QObject *context,
                                 const Ends &ends) {
    auto made = AiRegistry::answers(settings, {}, defaultsFile);
    if (!made)
        return std::unexpected(made.error());
    AiProvider *provider = made->get();
    const auto reply = std::make_shared<QString>();
    QObject::connect(provider, &AiProvider::delta, context, [reply](const QString &text) { *reply += text; });
    QObject::connect(provider, &AiProvider::finished, context, [reply, ends] { ends.said(reply->simplified()); });
    QObject::connect(provider, &AiProvider::failed, context,
                     [ends](const Failure &why) { ends.failed(CapabilityTest::whyOf(why.code()), why.text()); });
    provider->ask({.system = u"You are being tested. Reply with exactly two words and nothing else."_s,
                   .messages = {{.role = u"user"_s, .text = u"Say that you are working."_s}},
                   .tier = capability == Capability::Quick ? AiRequest::Quick : AiRequest::Main,
                   .model = {},
                   .prose = false});
    return Worker{.object = *std::move(made), .stop = [provider] { provider->cancel(); }};
}

// The first way the registry gives, as the row names it, and only that one.
[[nodiscard]] Started recognize(const Settings &settings, Recognition::Input input, const QObject *context, const Ends &ends) {
    auto ways = AiRegistry::recognizers(settings, input.kind, true);
    if (!ways || ways->empty())
        return std::unexpected(Failure::said(ways ? QString() : ways.error()));
    std::unique_ptr<Recognition::Recognizer> first = std::move(ways->front());
    Recognition::Recognizer *way = first.get();
    QObject::connect(way, &Recognition::Recognizer::resolved, context, [way, input = std::move(input)] { way->recognize(input); });
    QObject::connect(way, &Recognition::Recognizer::recognized, context,
                     [ends](const Recognition::Result &result) { ends.said(result.text.simplified()); });
    QObject::connect(way, &Recognition::Recognizer::failed, context, [ends](const Recognition::Failure &failure) {
        // One that is away and says no more is not there to be asked.
        const QString why = CapabilityTest::whyOf(failure.code);
        ends.failed(why == "failed"_L1 && failure.why == Recognition::Failure::Why::Away ? u"no-route"_s : why, failure.message);
    });
    way->resolve();
    return Worker{.object = std::move(first), .stop = [way] { way->cancel(); }};
}

[[nodiscard]] Started speak(const Settings &settings, const QObject *context, const Ends &ends) {
    auto made = AiRegistry::voice(settings);
    if (!made)
        return std::unexpected(made.error());
    Voice *voice = made->get();
    const QString sentence = QObject::tr("This is how your books will be read aloud.");
    QObject::connect(voice, &Voice::spoken, context, [sentence, ends] { ends.said(sentence); });
    QObject::connect(voice, &Voice::failed, context,
                     [ends](const Failure &why) { ends.failed(CapabilityTest::whyOf(why.code()), why.text()); });
    voice->say(sentence, Voices::speech(settings, QLocale().name()));
    return Worker{.object = *std::move(made), .stop = [voice] { voice->stop(); }};
}

using Made = std::expected<Embeddings::Vectors, Failure>;

[[nodiscard]] Started embed(const Settings &settings, std::chrono::milliseconds timeout, const QObject *context, const Ends &ends) {
    const auto route = AiRegistry::embeddings(settings);
    if (!route)
        return std::unexpected(route.error());
    auto watcher = std::make_unique<QFutureWatcher<Made>>();
    QFutureWatcher<Made> *watching = watcher.get();
    QObject::connect(watching, &QFutureWatcherBase::finished, context, [watching, ends] {
        const Made made = watching->result();
        if (!made)
            ends.failed(CapabilityTest::whyOf(made.error().code()), made.error().text());
        else
            ends.said(QString::number(made->empty() ? 0 : made->front().size()));
    });
    // Its own thread: embed waits for the answer. What it was given is its own copy.
    watching->setFuture(QtConcurrent::run(
        [route = *route, timeout] { return Embeddings::embed(route, {u"Omabook"_s}, Embeddings::Use::Question, timeout); }));
    return Worker{.object = std::move(watcher), .stop = {}};
}

// What every test is started with besides the settings.
struct Asking {
    QString defaultsFile;
    std::chrono::milliseconds timeout;
    QString clip; // where the clip speech to text hears is put
    const QObject *context;
    Ends ends;
};

[[nodiscard]] Started start(Capability capability, const Settings &settings, const Asking &asking) {
    switch (capability) {
    case Capability::Answers:
    case Capability::Quick:
        return askAnswers(settings, capability, asking.defaultsFile, asking.context, asking.ends);
    case Capability::Handwriting:
        return recognize(settings,
                         {.kind = Recognition::Kind::Ink, .strokes = CapabilityTest::drawnWord(), .file = {}, .language = u"en"_s},
                         asking.context, asking.ends);
    case Capability::SpeechToText:
        // The clip as a file of its own, as every way of hearing reads one.
        if (!QFile::exists(asking.clip) && !QFile::copy(u":/sounds/test-clip.wav"_s, asking.clip))
            return std::unexpected(Failure(Code::RecordingUnreadable, {asking.clip}));
        return recognize(settings, {.kind = Recognition::Kind::Audio, .strokes = {}, .file = asking.clip, .language = u"en"_s},
                         asking.context, asking.ends);
    case Capability::TextToSpeech:
        return speak(settings, asking.context, asking.ends);
    case Capability::Embeddings:
        return embed(settings, asking.timeout, asking.context, asking.ends);
    }
    return std::unexpected(Failure());
}

} // namespace

// One test under way: what it asks, the settings it asks by when they are a
// copy, the clip it hears, and the minute it is given.
struct CapabilityTest::Running {
    int generation = 0;
    QElapsedTimer clock;
    QTimer deadline;
    QTemporaryDir dir;
    std::unique_ptr<Settings> copy;
    Worker worker;

    Running() = default;
    Running(const Running &) = delete;
    Running &operator=(const Running &) = delete;
    Running(Running &&) = delete;
    Running &operator=(Running &&) = delete;
    ~Running() {
        // Nothing it says now is heard; its signal may still be on the way
        // out of it, and it may still read the copy.
        if (worker.object)
            worker.object->disconnect();
        if (worker.stop)
            worker.stop();
        if (worker.object)
            worker.object.release()->deleteLater();
        if (copy)
            copy.release()->deleteLater();
    }
};

CapabilityTest::CapabilityTest(Settings *settings, QString defaultsFile, QObject *parent)
    : QObject(parent), m_settings(settings), m_defaultsFile(std::move(defaultsFile)) {}

CapabilityTest::~CapabilityTest() = default;

QString CapabilityTest::whyOf(Failure::Code code) {
    const auto *const found = std::ranges::find(worded, code, &Worded::code);
    return found == worded.end() ? u"failed"_s : QString(found->why);
}

QString CapabilityTest::whyOf(CapabilityLack lack) {
    switch (lack) {
    case CapabilityLack::AiOff:
        return u"ai-off"_s;
    case CapabilityLack::HostedNotAllowed:
    case CapabilityLack::HostedNotChosen:
        return u"hosted-not-allowed"_s;
    case CapabilityLack::NoKey:
        return u"refused-key"_s;
    case CapabilityLack::NoModel:
        return u"no-model"_s;
    case CapabilityLack::NoServer:
    case CapabilityLack::NotInstalled:
    case CapabilityLack::Unreachable:
        return u"no-route"_s;
    case CapabilityLack::None:
        break;
    }
    return u"failed"_s;
}

std::vector<Ink::Stroke> CapabilityTest::drawnWord() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto stroke = [now](int at, const std::vector<QPointF> &points) {
        Ink::Stroke drawn;
        drawn.id = u"test-%1"_s.arg(at);
        drawn.width = 4.0;
        drawn.began = now + qint64{at} * 400;
        quint32 ms = 0;
        for (const QPointF &point : points) {
            drawn.points.push_back({.at = point, .pressure = 0.5F, .xTilt = 0.0F, .yTilt = 0.0F, .ms = ms});
            ms += 12;
        }
        return drawn;
    };
    // The O, round, then the K's upright and its two arms.
    std::vector<QPointF> round;
    for (int step = 0; step <= 36; ++step) {
        const double angle = qDegreesToRadians(step * 10.0);
        round.emplace_back(40.0 + 30.0 * std::sin(angle), 50.0 - 40.0 * std::cos(angle));
    }
    return {stroke(0, round), stroke(1, {{100.0, 10.0}, {100.0, 50.0}, {100.0, 90.0}}),
            stroke(2, {{140.0, 10.0}, {120.0, 35.0}, {101.0, 55.0}}), stroke(3, {{110.0, 45.0}, {125.0, 68.0}, {142.0, 90.0}})};
}

void CapabilityTest::run(Capability capability, bool hostedOnce, bool fallback) {
    const Which which{capability, fallback};
    m_running.erase(which);
    // A test asks again whether the servers are there, rather than go by what was found a while ago.
    ServerReach::forget();
    const int generation = ++m_generation;
    const auto end = [this, which, generation](QVariantMap outcome) {
        // Never while it is being started: a test ends after it began.
        QMetaObject::invokeMethod(
            this, [this, which, generation, outcome = std::move(outcome)] { finish(which, generation, outcome); }, Qt::QueuedConnection);
    };
    const Ends ends{.said = [end](const QString &said) { end({{u"state"_s, u"passed"_s}, {u"said"_s, said}}); },
                    .failed = [end](const QString &why, const QString &message) { end(failedWith(why, message)); }};
    auto running = std::make_unique<Running>();
    running->generation = generation;
    running->clock.start();
    running->deadline.setSingleShot(true);
    // Queued, as the timer goes with the test it ends.
    connect(
        &running->deadline, &QTimer::timeout, this, [this, which, generation] { finish(which, generation, failedWith(u"timeout"_s, {})); },
        Qt::QueuedConnection);
    running->deadline.start(m_timeout);

    const Settings *used = m_settings;
    if (fallback) {
        // The fallback, as though the first were away (#174): hosted only by its own allowance, or this one yes.
        running->copy = copyOf(*m_settings, running->dir);
        if (running->copy) {
            AiFallback::tryInstead(*running->copy, capability);
            if (hostedOnce)
                AiFallback::choose(*running->copy, capability, {{u"hosted"_s, true}});
            used = running->copy.get();
        }
    } else if (hostedOnce && !m_settings->aiOff() && !AiPrivacy::hostedAllowed(*m_settings, capability)) {
        running->copy = hostedCopy(*m_settings, capability, running->dir);
        if (running->copy)
            used = running->copy.get();
    }
    // Why it can't be used is the test's answer, and nothing is sent: with
    // the AI Off, a hosted provider not allowed, nothing set up.
    const CapabilityStatus status = AiRegistry::status(*used, capability, {}, m_defaultsFile);
    if (m_settings->aiOff()) {
        end(failedWith(u"ai-off"_s, Failure(Code::AiOff).text()));
    } else if (!status.available) {
        end(failedWith(whyOf(status.why), status.failure.text()));
    } else if (fallback && status.insteadOf.isEmpty()) {
        end(failedWith(u"no-route"_s, Failure(Code::NoFallback).text()));
    } else {
        Started started = start(capability, *used,
                                {.defaultsFile = m_defaultsFile,
                                 .timeout = m_timeout,
                                 .clip = running->dir.filePath(u"test-clip.wav"_s),
                                 .context = this,
                                 .ends = ends});
        if (started)
            running->worker = *std::move(started);
        else
            end(failedWith(whyOf(started.error().code()), started.error().text()));
    }
    m_running[which] = std::move(running);
    m_outcomes[which] = {{u"state"_s, u"running"_s}};
    emit changed();
}

void CapabilityTest::finish(Which which, int generation, QVariantMap outcome) {
    const auto found = m_running.find(which);
    if (found == m_running.end() || found->second->generation != generation)
        return; // stopped, or tested again since
    outcome.insert(u"ms"_s, found->second->clock.elapsed());
    m_running.erase(found);
    m_outcomes[which] = std::move(outcome);
    emit changed();
}

void CapabilityTest::stop() {
    m_running.clear();
    m_outcomes.clear();
    emit changed();
}

QVariantMap CapabilityTest::outcome(Capability capability, bool fallback) const {
    const auto found = m_outcomes.find({capability, fallback});
    return found == m_outcomes.end() ? QVariantMap() : found->second;
}
