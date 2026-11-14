#include "converter.h"

#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

Converter::Converter(const QString &program, QObject *parent)
    : QObject(parent), m_program(program) {
    m_process.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_locker.reset();
        const bool written = code == 0 && status == QProcess::NormalExit && QFileInfo(m_output).size() > 0;
        if (!written)
            QFile::remove(m_output);
        // The tool's last line is its reason, when it gives one.
        const QString last = QString::fromUtf8(m_process.readAll()).trimmed().section('\n', -1).trimmed();
        emit finished(m_output, written ? QString() : last.isEmpty() ? "The conversion failed." : "The conversion failed: " + last);
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return; // anything later also arrives as finished
        m_locker.reset();
        emit finished(m_output, "The converter can't be started: " + m_program);
    });
}

QString Converter::find() { return QStandardPaths::findExecutable("ebook-convert"); }

void Converter::setProgram(const QString &program, const QString &why) {
    m_program = program;
    m_why = why;
}

void Converter::convert(const QString &input, const QString &output) {
    m_output = output;
    if (!available()) {
        emit finished(output, m_why.isEmpty() ? "Reading this format needs Calibre's ebook-convert, which isn't installed." : m_why);
        return;
    }
    if (running()) {
        emit finished(output, "Another book is still being converted.");
        return;
    }
    m_locker = std::make_unique<QEventLoopLocker>();
    m_process.start(m_program, {input, output});
}

void Converter::cancel() {
    if (running())
        m_process.kill();
}
