#pragma once

#include <QEventLoopLocker>
#include <QObject>
#include <QProcess>
#include <memory>

// Formats Omabook doesn't read itself (Kindle, Word, DjVu, ...) are turned
// into EPUB by Calibre's ebook-convert, the way video apps lean on ffmpeg. It
// is an optional tool: without it those books still live in the library, and
// opening one says what is missing. Tests pass a stand-in program.
class Converter : public QObject {
    Q_OBJECT

public:
    // `program` is the converter to run, empty when there is none.
    explicit Converter(const QString &program, QObject *parent = nullptr);
    // Where ebook-convert is installed, or empty. The app asks the Calibre
    // plugin instead (CalibrePlugin::converter), which also knows whether
    // Calibre is switched off.
    static QString find();
    // Changes the converter, as when the Calibre plugin is switched; `why`
    // is the reason there is none, in English, for whoever asks to convert.
    void setProgram(const QString &program, const QString &why = {});

    bool available() const { return !m_program.isEmpty(); }
    bool running() const { return m_process.state() != QProcess::NotRunning; }
    // Writes `output` from `input`; the format is the output's extension.
    void convert(const QString &input, const QString &output);
    void cancel();

signals:
    // `error` is empty when the output was written; otherwise English.
    void finished(const QString &output, const QString &error);

private:
    QString m_program;
    QString m_why;
    QString m_output;
    QProcess m_process;
    // Keeps the app alive, windowless, until a running conversion is done.
    std::unique_ptr<QEventLoopLocker> m_locker;
};
