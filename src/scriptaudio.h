#pragma once

#include <QJSValue>
#include <QObject>
#include <QString>
#include <QTimer>
#include <map>
#include <memory>

#include "audiodevices.h"
#include "audioplayer.h"
#include "audiorecorder.h"

class ScriptPlugin;

// The C++ side of `omabook.audio` (#79), built in scriptaudio.js, which the
// script sees and this never is: recording from the microphone into a file
// under the plugin's data/ folder, as Opus, and playing such a file back.
//
// A plugin records only with the `microphone` permission, with the desktop
// letting Omabook use the microphone, and once the person said yes to it in
// the window (asked the first time it records while it is loaded); and only
// while the window shows that it records (AudioDevices), where the person
// can stop it. Where nobody can be asked (a command), it can't record.
class ScriptAudio : public QObject {
    Q_OBJECT

public:
    // `devices` may be null: then the desktop's own, with nobody to show it.
    ScriptAudio(ScriptPlugin *plugin, AudioDevices *devices);
    ~ScriptAudio() override;
    ScriptAudio(const ScriptAudio &) = delete;
    ScriptAudio &operator=(const ScriptAudio &) = delete;
    ScriptAudio(ScriptAudio &&) = delete;
    ScriptAudio &operator=(ScriptAudio &&) = delete;

    // The `omabook.audio` object, made once.
    [[nodiscard]] QJSValue api();

    // Throws into the script when `file` is no place to record to.
    Q_INVOKABLE void check(const QString &file);
    // Asks the person, once, whether the plugin may record: `answered` is
    // given "{}" for yes and null for no, and refusal() then says why.
    Q_INVOKABLE void ask(const QJSValue &answered);
    Q_INVOKABLE QString refusal() const { return m_refusal; }
    // Each of what answers a string answers JSON, which the wrapper parses.
    Q_INVOKABLE int record(const QString &file, const QJSValue &onLevel, const QJSValue &onStopped);
    Q_INVOKABLE QString recordingState(int id) const;
    Q_INVOKABLE void pause(int id);
    Q_INVOKABLE void resume(int id);
    Q_INVOKABLE QString stop(int id);
    Q_INVOKABLE int open(const QString &file, double at, const QJSValue &onPosition, const QJSValue &onEnded, bool paused);
    Q_INVOKABLE QString playerState(int id) const;
    Q_INVOKABLE void play(int id);
    Q_INVOKABLE void pausePlaying(int id);
    Q_INVOKABLE void seek(int id, double at);
    Q_INVOKABLE void stopPlaying(int id);
    Q_INVOKABLE QString info(const QString &file);

private:
    struct Recording {
        std::unique_ptr<AudioRecorder> recorder;
        QString file; // as the script named it
        QJSValue onLevel, onStopped;
        double heard = -1; // the level onLevel was last given
    };
    struct Playing {
        std::unique_ptr<AudioPlayer> player;
        QString file;
        QJSValue onPosition, onEnded;
        qint64 told = -1; // the position onPosition was last given
    };

    void tick();
    void keepTicking();
    // A recording ended by itself or from the window: the plugin is told.
    void ended(int id);
    void callBack(const QJSValue &function, const QJSValueList &arguments);
    void fail(const QString &why);
    void consent(const QJSValue &answered);
    // The file's real path inside the plugin's folder, or empty with an error thrown.
    [[nodiscard]] QString pathOf(const QString &file, bool writing);
    [[nodiscard]] Playing *player(int id);

    ScriptPlugin *m_plugin;
    std::unique_ptr<AudioDevices> m_own;
    AudioDevices *m_devices;
    std::map<int, Recording> m_recordings;
    std::map<int, Playing> m_players;
    int m_next = 1;
    QTimer m_ticks;
    bool m_consented = false;
    QString m_refusal;
    QJSValue m_api;
};
