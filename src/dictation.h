#pragma once

#include <QObject>
#include <QString>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariantMap>
#include <memory>

#include "audiodevices.h"
#include "dictationclip.h"
#include "recognitionqueue.h"
#include "voxtypedictation.h"

class Settings;

// Dictation (#148): what the person says into a note field, written out by
// the speech-to-text capability (ADR 0008) and put in the field where its
// cursor is. One field at a time is listened for, by its number: the field
// asks to be (`listen`), says when the person is done (`stop`), and is given
// the words (`heard`) or why there are none (`failed`).
//
// It listens only when a field asks, which a field does only when the person
// pressed its microphone or its key, and while it listens the window shows
// it (`listening`, `who`, `level`), whatever else is open, with a way to stop.
// What is said is kept in memory, written for the way that writes it out to
// a folder of its own, and gone once it is written out: nothing of it is kept
// in the library.
//
// It goes the ways the registry gives speech to text: voxtype first, a hosted
// one only where the settings allow it for speech to text and the one asking
// asked (Omabook's own fields do; a plugin's view says so itself), and none
// with the AI Off. Where the person chose it (`recognition.dictation`:
// "voxtype"), voxtype's own dictation listens instead and types into the
// field (VoxtypeDictation), still on this computer, still never with the AI Off.
class Dictation : public QObject {
    Q_OBJECT
    // Listening now, and for whom: a plugin's name, empty for Omabook itself.
    Q_PROPERTY(bool listening READ listening NOTIFY changed)
    Q_PROPERTY(QString who READ who NOTIFY changed)
    // What was said is being written out.
    Q_PROPERTY(bool writing READ writing NOTIFY changed)
    // The field listened for or written for; 0 for none.
    Q_PROPERTY(int field READ field NOTIFY changed)
    // It is voxtype's own dictation that listens, and types the words itself.
    Q_PROPERTY(bool byVoxtype READ byVoxtype NOTIFY changed)
    // It listens for a command said while reading (#150), not for a field.
    Q_PROPERTY(bool forCommand READ forCommand NOTIFY changed)
    // How long it has listened, in milliseconds, and how loud it hears, 0 to 1.
    Q_PROPERTY(double elapsed READ elapsed NOTIFY progressed)
    Q_PROPERTY(double level READ level NOTIFY progressed)
    // voxtype is on this computer, and whether its own dictation is chosen.
    Q_PROPERTY(bool voxtypeFound READ voxtypeFound NOTIFY settingsChanged)
    Q_PROPERTY(bool withVoxtype READ withVoxtype WRITE setWithVoxtype NOTIFY settingsChanged)

public:
    // The ways speech is written out are the registry's, by the settings.
    Dictation(Settings *settings, AudioDevices *devices, QObject *parent = nullptr);
    // The same, with the ways `route` gives (a test's).
    Dictation(Settings *settings, AudioDevices *devices, RecognitionQueue::Route route, QObject *parent = nullptr);
    ~Dictation() override;
    Dictation(const Dictation &) = delete;
    Dictation &operator=(const Dictation &) = delete;
    Dictation(Dictation &&) = delete;
    Dictation &operator=(Dictation &&) = delete;

    // Whether a field may be dictated into, `hosted` as the one asking says
    // it: the registry's status of speech to text ({available, hosted,
    // provider, model, why, message}), and `voxtype`, true where voxtype's
    // own dictation would listen. Where the words are wanted back, not
    // typed (`typing` false, for a command), voxtype's own never listens.
    Q_INVOKABLE QVariantMap status(bool hosted, bool typing = true) const;
    // Listens for a new field, by `who` (a plugin's name, empty for Omabook),
    // a hosted way only if `hosted`. Its number; failed() says, a moment
    // later, when it couldn't start.
    Q_INVOKABLE int listen(const QString &who, bool hosted);
    // Listens for a command said while reading (#150): Omabook's own, its
    // words given back by heard(), never typed by voxtype's own dictation.
    Q_INVOKABLE int listenForCommand();
    // The person is done speaking: what they said is written out.
    Q_INVOKABLE void stop();
    // Stops listening and writing out, and nothing is put in the field.
    Q_INVOKABLE void cancel();
    // The field went: if it was listened or written for, that stops too.
    Q_INVOKABLE void forget(int field);

    [[nodiscard]] bool listening() const { return m_byVoxtype || (m_clip && m_clip->listening()); }
    [[nodiscard]] QString who() const { return m_who; }
    [[nodiscard]] bool writing() const { return m_job != 0; }
    [[nodiscard]] int field() const { return m_field; }
    [[nodiscard]] bool byVoxtype() const { return m_byVoxtype; }
    [[nodiscard]] bool forCommand() const { return m_forCommand; }
    [[nodiscard]] double elapsed() const;
    [[nodiscard]] double level() const { return m_clip ? m_clip->level() : 0.0; }
    [[nodiscard]] bool voxtypeFound() const;
    [[nodiscard]] bool withVoxtype() const;
    void setWithVoxtype(bool on);

signals:
    // The words said for the field, to be put where its cursor is.
    void heard(int field, const QString &text);
    // Why there are none, in words.
    void failed(int field, const QString &why);
    // voxtype has what was said, and types it into the field itself.
    void handedOver(int field);
    void changed();
    void progressed();
    void settingsChanged();

private:
    void fail(int field, const QString &why);
    void ended(const QString &why);
    void writeOut();
    void reset();
    int start(const QString &who, bool hosted, bool typing);

    Settings *m_settings;
    AudioDevices *m_devices;
    RecognitionQueue m_queue;
    VoxtypeDictation m_voxtype;
    int m_voxtypeField = 0; // the last field voxtype's own dictation was for
    std::unique_ptr<DictationClip> m_clip;
    std::unique_ptr<QTemporaryDir> m_folder;
    QTimer m_ticks;
    QString m_who;
    bool m_hosted = false;
    bool m_byVoxtype = false;
    bool m_forCommand = false;
    int m_field = 0;
    int m_job = 0; // the queue's, while what was said is written out
    int m_next = 1;
};
