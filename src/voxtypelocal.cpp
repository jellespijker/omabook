#include "voxtypelocal.h"

#include <QDateTime>
#include <QFileInfo>
#include <utility>

#include "failure.h"
#include "localspeech.h"

using namespace Qt::StringLiterals;

namespace {

using Why = Recognition::Failure::Why;

[[nodiscard]] QString converter() { return QString::fromLatin1(LocalSpeech::converter); }

} // namespace

VoxtypeLocal::VoxtypeLocal(QString program, QString model, QObject *parent)
    : Recognizer(parent), m_program(std::move(program)), m_model(std::move(model)) {
    connect(&m_process, &QProcess::finished, this, &VoxtypeLocal::read);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            emit failed({.why = Why::Unsupported,
                         .message = m_converting ? ::Failure(::Failure::Code::VoxtypeNeedsWav, {m_recording}).text() : missing(m_program)});
    });
}

VoxtypeLocal::~VoxtypeLocal() { cancel(); }

QString VoxtypeLocal::missing(const QString &program) {
    return LocalSpeech::found(program).isEmpty() ? ::Failure(::Failure::Code::VoxtypeMissing, {program}).text() : QString();
}

QString VoxtypeLocal::where() const { return tr("voxtype on this computer"); }

QString VoxtypeLocal::model() const { return m_model.isEmpty() ? QString::fromLatin1(defaultProgram) : m_model; }

void VoxtypeLocal::recognize(const Recognition::Input &input) {
    cancel();
    if (const QString why = missing(m_program); !why.isEmpty()) {
        emit failed({.why = Why::Unsupported, .message = why});
        return;
    }
    m_folder = std::make_unique<QTemporaryDir>();
    m_language = input.language;
    m_recording = QFileInfo(input.file).fileName();
    if (LocalSpeech::isWav(input.file)) {
        transcribe(input.file);
        return;
    }
    if (LocalSpeech::found(converter()).isEmpty()) {
        emit failed({.why = Why::Unsupported, .message = ::Failure(::Failure::Code::VoxtypeNeedsWav, {m_recording}).text()});
        return;
    }
    m_converting = true;
    m_process.start(converter(), LocalSpeech::conversion(input.file, m_folder->filePath(u"recording.wav"_s)));
}

void VoxtypeLocal::transcribe(const QString &wav) {
    m_converting = false;
    // The language and the model are voxtype's own options, before the command.
    QStringList arguments;
    if (!m_language.isEmpty())
        arguments << u"--language"_s << m_language;
    if (!m_model.isEmpty())
        arguments << u"--model"_s << m_model;
    arguments << u"transcribe"_s << wav;
    m_process.start(LocalSpeech::found(m_program), arguments);
}

void VoxtypeLocal::read() {
    if (!m_folder)
        return; // cancelled
    const bool worked = m_process.exitStatus() == QProcess::NormalExit && m_process.exitCode() == 0;
    const QString said = QString::fromUtf8(m_process.readAllStandardError()).trimmed().section(u'\n', -1);
    if (!worked) {
        // voxtype without its model, say: the next way is tried.
        emit failed({.why = m_converting ? Why::Refused : Why::Unsupported,
                     .message = m_converting ? ::Failure(::Failure::Code::FfmpegFailed, {said}).text()
                                             : ::Failure(::Failure::Code::VoxtypeFailed, {said}).text()});
        return;
    }
    if (m_converting) {
        transcribe(m_folder->filePath(u"recording.wav"_s));
        return;
    }
    Recognition::Result result = Voxtype::transcript(QString::fromUtf8(m_process.readAllStandardOutput()));
    result.language = m_language;
    result.model = model();
    result.server = where();
    m_folder.reset();
    emit recognized(result);
}

void VoxtypeLocal::cancel() {
    m_folder.reset();
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(1000);
    }
}

Recognition::Result Voxtype::transcript(const QString &output) {
    QString lines = output;
    lines.replace("\r\n"_L1, "\n"_L1);
    const qsizetype gap = lines.indexOf("\n\n"_L1);
    const QString text = gap < 0 ? QString() : lines.mid(gap + 2).trimmed();
    Recognition::Result result{.kind = Recognition::Kind::Audio,
                               .text = text,
                               .segments = {},
                               .language = {},
                               .model = {},
                               .server = {},
                               .at = QDateTime::currentMSecsSinceEpoch(),
                               .kept = false,
                               .timed = false};
    if (!text.isEmpty())
        result.segments.append({.text = text, .start = 0, .end = 0, .words = {}});
    return result;
}
