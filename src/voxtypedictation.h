#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

// voxtype's own dictation (#148), started from Omabook: `voxtype record
// start` has voxtype's daemon listen, `stop` has it write out what it heard
// and type it into the window that has the keyboard (Omabook's, its field
// still focused), and `cancel` drops it. voxtype listens and types, as it
// does for any program; Omabook shows that it listens and stops it. It types
// the words alone: no Enter pressed after them, so a note isn't sent, and no
// overlay of voxtype's over Omabook's own.
class VoxtypeDictation : public QObject {
    Q_OBJECT

public:
    explicit VoxtypeDictation(QObject *parent = nullptr);
    ~VoxtypeDictation() override;
    VoxtypeDictation(const VoxtypeDictation &) = delete;
    VoxtypeDictation &operator=(const VoxtypeDictation &) = delete;
    VoxtypeDictation(VoxtypeDictation &&) = delete;
    VoxtypeDictation &operator=(VoxtypeDictation &&) = delete;

    // What `voxtype record` is given for each step.
    [[nodiscard]] static QStringList arguments(const QString &step);

    // The command each step runs: the settings', else "voxtype" on the path.
    void setProgram(const QString &program) { m_program = program; }
    void start() { run(QStringLiteral("start")); }
    void stop() { run(QStringLiteral("stop")); }
    void cancel() { run(QStringLiteral("cancel")); }

signals:
    // voxtype said no (its daemon isn't running, say), in words.
    void failed(const QString &why);

private:
    void run(const QString &step);
    void finished();

    QString m_program;
    QProcess m_process;
    QStringList m_waiting; // steps asked while one runs, in order
};
