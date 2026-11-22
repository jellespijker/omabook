#include "voxtypedictation.h"

#include "failure.h"
#include "localspeech.h"

using namespace Qt::StringLiterals;

VoxtypeDictation::VoxtypeDictation(QObject *parent) : QObject(parent) {
    connect(&m_process, &QProcess::finished, this, &VoxtypeDictation::finished);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        m_waiting.clear();
        emit failed(Failure(Failure::Code::VoxtypeMissing, {m_program}).text());
    });
}

VoxtypeDictation::~VoxtypeDictation() {
    if (m_process.state() != QProcess::NotRunning)
        m_process.waitForFinished(1000);
}

QStringList VoxtypeDictation::arguments(const QString &step) {
    QStringList arguments{u"record"_s, step};
    if (step == "start"_L1)
        arguments << u"--type"_s << u"--no-auto-submit"_s << u"--no-osd"_s;
    return arguments;
}

void VoxtypeDictation::run(const QString &step) {
    if (m_process.state() != QProcess::NotRunning) {
        m_waiting << step;
        return;
    }
    const QString program = LocalSpeech::found(m_program);
    if (program.isEmpty()) {
        m_waiting.clear();
        emit failed(Failure(Failure::Code::VoxtypeMissing, {m_program}).text());
        return;
    }
    m_process.start(program, arguments(step));
}

void VoxtypeDictation::finished() {
    if (m_process.exitStatus() != QProcess::NormalExit || m_process.exitCode() != 0) {
        const QString said = QString::fromUtf8(m_process.readAllStandardError() + m_process.readAllStandardOutput()).trimmed();
        m_waiting.clear();
        emit failed(Failure(Failure::Code::VoxtypeDictationFailed, {said.section(u'\n', -1)}).text());
        return;
    }
    if (!m_waiting.isEmpty())
        run(m_waiting.takeFirst());
}
