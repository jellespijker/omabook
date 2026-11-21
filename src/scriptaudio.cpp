#include "scriptaudio.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaMethod>
#include <QPermissions>
#include <QVector>
#include <ranges>
#include <tuple>

#include "opusreader.h"
#include "scriptplugin.h"

using namespace Qt::StringLiterals;

namespace {

// How often a plugin hears the level and the position: often enough for a
// meter and a view that follows along.
constexpr std::chrono::milliseconds tickEvery{100};
// Players a plugin has open at once; one more throws until it stops one.
constexpr std::size_t mostPlayers = 8;

[[nodiscard]] QString compact(const QJsonObject &object) { return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)); }

[[nodiscard]] QString moment(const QDateTime &time) { return time.isValid() ? time.toUTC().toString(Qt::ISODateWithMs) : QString(); }

} // namespace

ScriptAudio::ScriptAudio(ScriptPlugin *plugin, AudioDevices *devices)
    : m_plugin(plugin), m_own(devices != nullptr ? nullptr : std::make_unique<AudioDevices>()),
      m_devices(devices != nullptr ? devices : m_own.get()) {
    QJSEngine::setObjectOwnership(this, QJSEngine::CppOwnership);
    m_ticks.setInterval(tickEvery);
    connect(&m_ticks, &QTimer::timeout, this, &ScriptAudio::tick);
    connect(m_devices, &AudioDevices::stopAsked, this, [this] {
        const auto ids = m_recordings | std::views::keys | std::ranges::to<QVector<int>>();
        for (const int id : ids)
            ended(id);
    });
}

ScriptAudio::~ScriptAudio() {
    // The plugin is going: what it recorded is kept, ended as it should be,
    // and the window no longer shows it recording. Its script hears nothing.
    for (auto &[id, recording] : m_recordings) {
        recording.recorder->stop();
        m_devices->ended(recording.recorder.get());
    }
}

QJSValue ScriptAudio::api() {
    if (!m_api.isUndefined())
        return m_api;
    QFile wrapper(u":/scriptaudio.js"_s);
    if (!wrapper.open(QIODevice::ReadOnly))
        return {};
    QJSEngine &engine = m_plugin->m_engine;
    const QJSValue build = engine.evaluate(QString::fromUtf8(wrapper.readAll()), u"omabook-api-audio"_s);
    m_api = build.call({engine.newQObject(this), QJSValue(m_plugin->m_manifest.permissions.contains(u"microphone"_s))});
    return m_api;
}

void ScriptAudio::fail(const QString &why) { m_plugin->m_engine.throwError(why); }

void ScriptAudio::callBack(const QJSValue &function, const QJSValueList &arguments) {
    if (!function.isCallable())
        return;
    QString problem;
    m_plugin->call(function, arguments, &problem);
    if (!problem.isEmpty())
        m_plugin->report(problem);
}

QString ScriptAudio::pathOf(const QString &file, bool writing) {
    const QString path = m_plugin->inside(file, writing);
    if (path.isEmpty())
        fail(writing ? u"A recording goes under the plugin's data/ folder (\"data/recordings/monday.opus\"), not %1."_s.arg(file)
                     : u"%1 is not a file in the plugin's folder."_s.arg(file));
    return path;
}

void ScriptAudio::check(const QString &file) {
    if (!file.endsWith(u".opus"_s)) {
        fail(u"A recording is kept as Opus: its file ends in .opus, which %1 doesn't."_s.arg(file));
        return;
    }
    std::ignore = pathOf(file, true); // what is wrong with it has been thrown
}

void ScriptAudio::ask(const QJSValue &answered) {
    // Said beforehand for a test (ScriptServices::microphoneGranted): neither the desktop nor the person is asked.
    if (m_consented || m_plugin->m_services.microphoneGranted) {
        answered.call({QJSValue(u"{}"_s)});
        return;
    }
#if QT_CONFIG(permissions)
    // The desktop's word comes first, where it has one.
    const QMicrophonePermission microphone;
    switch (qApp->checkPermission(microphone)) {
    case Qt::PermissionStatus::Denied:
        m_refusal = u"The desktop doesn't let Omabook use the microphone."_s;
        answered.call({QJSValue(QJSValue::NullValue)});
        return;
    case Qt::PermissionStatus::Undetermined:
        qApp->requestPermission(microphone, this, [this, answered](const QPermission &asked) {
            if (asked.status() == Qt::PermissionStatus::Granted) {
                consent(answered);
                return;
            }
            m_refusal = u"The desktop doesn't let Omabook use the microphone."_s;
            callBack(answered, {QJSValue(QJSValue::NullValue)});
        });
        return;
    case Qt::PermissionStatus::Granted:
        break;
    }
#endif
    consent(answered);
}

// The person's own word, in the window, as a form the plugin can't change.
void ScriptAudio::consent(const QJSValue &answered) {
    static const QMetaMethod asking = QMetaMethod::fromSignal(&ScriptPlugin::formRequested);
    if (!m_plugin->isSignalConnected(asking)) {
        m_refusal = u"Nobody is here to say yes to recording: a plugin records only in the window."_s;
        answered.call({QJSValue(QJSValue::NullValue)});
        return;
    }
    m_refusal = u"The person didn't let the plugin record."_s;
    const QString name = m_plugin->m_manifest.name.isEmpty() ? m_plugin->m_manifest.id : m_plugin->m_manifest.name;
    const QJsonObject form{
        {"title", tr("Let %1 use the microphone?").arg(name)},
        {"intro", tr("It records what the microphone hears into its own folder until it stops. While it records, a red mark at "
                     "the top of the window says so, with a button to stop it.")},
        {"accept", tr("Allow")},
        {"fields", QJsonArray()},
    };
    const int token = m_plugin->m_nextToken++;
    m_plugin->m_asks.insert(token, answered);
    m_plugin->busy(1);
    emit m_plugin->formRequested(token, form);
}

int ScriptAudio::record(const QString &file, const QJSValue &onLevel, const QJSValue &onStopped) {
    if (!m_recordings.empty()) {
        fail(u"The plugin is recording already: stop that first."_s);
        return 0;
    }
    const QString path = pathOf(file, true);
    if (path.isEmpty())
        return 0;
    // Only the wrapper calls this, after a yes.
    m_consented = true;
    auto recorder = AudioRecorder::start(path, m_devices->microphone(), QDateTime::currentDateTimeUtc());
    if (!recorder) {
        fail(recorder.error());
        return 0;
    }
    const int id = m_next++;
    connect(recorder->get(), &AudioRecorder::stopped, this, [this, id] { ended(id); });
    m_devices->began(recorder->get(), m_plugin->m_manifest.name.isEmpty() ? m_plugin->m_manifest.id : m_plugin->m_manifest.name);
    m_recordings.emplace(id, Recording{.recorder = std::move(*recorder), .file = file, .onLevel = onLevel, .onStopped = onStopped});
    keepTicking();
    return id;
}

QString ScriptAudio::recordingState(int id) const {
    const auto found = m_recordings.find(id);
    if (found == m_recordings.end())
        return compact({{"recording", false}});
    const AudioRecorder &recorder = *found->second.recorder;
    return compact({{"file", found->second.file},
                    {"started", moment(recorder.started())},
                    {"elapsed", static_cast<double>(recorder.elapsed().count())},
                    {"level", recorder.level()},
                    {"paused", recorder.paused()},
                    {"recording", recorder.recording()}});
}

void ScriptAudio::pause(int id) {
    if (const auto found = m_recordings.find(id); found != m_recordings.end()) {
        found->second.recorder->pause();
        m_devices->tick();
    }
}

void ScriptAudio::resume(int id) {
    if (const auto found = m_recordings.find(id); found != m_recordings.end()) {
        found->second.recorder->resume(QDateTime::currentDateTimeUtc());
        m_devices->tick();
    }
}

QString ScriptAudio::stop(int id) {
    const auto found = m_recordings.find(id);
    if (found == m_recordings.end())
        return u"null"_s;
    AudioRecorder::Summary summary = found->second.recorder->stop();
    summary.file = found->second.file;
    m_devices->ended(found->second.recorder.get());
    m_recordings.erase(found);
    keepTicking();
    return compact(summary.toJson());
}

void ScriptAudio::ended(int id) {
    const auto found = m_recordings.find(id);
    if (found == m_recordings.end())
        return;
    AudioRecorder::Summary summary = found->second.recorder->stop();
    summary.file = found->second.file;
    const QJSValue told = found->second.onStopped;
    m_devices->ended(found->second.recorder.get());
    m_recordings.erase(found);
    keepTicking();
    callBack(told, {m_plugin->fromJson(summary.toJson())});
}

int ScriptAudio::open(const QString &file, double at, const QJSValue &onPosition, const QJSValue &onEnded, bool paused) {
    if (m_players.size() >= mostPlayers) {
        fail(u"The plugin has %1 recordings open to play: stop one first."_s.arg(mostPlayers));
        return 0;
    }
    const QString path = pathOf(file, false);
    if (path.isEmpty())
        return 0;
    auto player = AudioPlayer::open(path, m_devices->speakers());
    if (!player) {
        fail(player.error());
        return 0;
    }
    const int id = m_next++;
    AudioPlayer *playing = player->get();
    connect(playing, &AudioPlayer::ended, this, [this, id] {
        if (Playing *found = this->player(id)) {
            found->told = found->player->position().count();
            callBack(found->onPosition, {QJSValue(static_cast<double>(found->told))});
            callBack(found->onEnded, {});
        }
    });
    connect(playing, &AudioPlayer::failed, this, [this](const QString &why) { m_plugin->report(why); });
    m_players.emplace(id, Playing{.player = std::move(*player), .file = file, .onPosition = onPosition, .onEnded = onEnded});
    if (auto sought = playing->seek(std::chrono::milliseconds(static_cast<qint64>(at))); !sought)
        fail(sought.error());
    else if (!paused)
        play(id);
    return id;
}

ScriptAudio::Playing *ScriptAudio::player(int id) {
    const auto found = m_players.find(id);
    return found != m_players.end() ? &found->second : nullptr;
}

QString ScriptAudio::playerState(int id) const {
    const auto found = m_players.find(id);
    if (found == m_players.end())
        return compact({{"playing", false}});
    const AudioPlayer &player = *found->second.player;
    return compact({{"file", found->second.file},
                    {"started", moment(player.started())},
                    {"duration", static_cast<double>(player.duration().count())},
                    {"position", static_cast<double>(player.position().count())},
                    {"playing", player.playing()}});
}

void ScriptAudio::play(int id) {
    if (Playing *found = player(id))
        if (auto played = found->player->play(); !played)
            fail(played.error());
    keepTicking();
}

void ScriptAudio::pausePlaying(int id) {
    if (Playing *found = player(id))
        found->player->pause();
}

void ScriptAudio::seek(int id, double at) {
    if (Playing *found = player(id))
        if (auto sought = found->player->seek(std::chrono::milliseconds(static_cast<qint64>(at))); !sought)
            fail(sought.error());
}

void ScriptAudio::stopPlaying(int id) {
    if (const auto found = m_players.find(id); found != m_players.end()) {
        found->second.player->stop();
        m_players.erase(found);
    }
    keepTicking();
}

QString ScriptAudio::info(const QString &file) {
    const QString path = pathOf(file, false);
    if (path.isEmpty())
        return u"null"_s;
    const auto reader = Opus::Reader::open(path);
    if (!reader) {
        fail(reader.error());
        return u"null"_s;
    }
    return compact({{"file", file},
                    {"started", moment((*reader)->started())},
                    {"duration", static_cast<double>((*reader)->duration().count())},
                    {"bytes", static_cast<double>(QFile(path).size())}});
}

void ScriptAudio::keepTicking() {
    if (m_recordings.empty() && m_players.empty())
        m_ticks.stop();
    else if (!m_ticks.isActive())
        m_ticks.start();
}

void ScriptAudio::tick() {
    if (!m_recordings.empty())
        m_devices->tick();
    // Copied first: a callback may stop what it is told about.
    const auto recordings = m_recordings | std::views::keys | std::ranges::to<QVector<int>>();
    for (const int id : recordings) {
        const auto found = m_recordings.find(id);
        if (found == m_recordings.end() || found->second.recorder->paused())
            continue;
        const double level = found->second.recorder->level();
        if (level == found->second.heard)
            continue;
        found->second.heard = level;
        callBack(found->second.onLevel, {QJSValue(level), QJSValue(static_cast<double>(found->second.recorder->elapsed().count()))});
    }
    const auto players = m_players | std::views::keys | std::ranges::to<QVector<int>>();
    for (const int id : players) {
        Playing *found = player(id);
        if (found == nullptr || !found->player->playing())
            continue;
        const qint64 position = found->player->position().count();
        if (position == found->told)
            continue;
        found->told = position;
        callBack(found->onPosition, {QJSValue(static_cast<double>(position))});
    }
}
