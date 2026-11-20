#include "whisperlocal.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>
#include <utility>

using namespace Qt::StringLiterals;

namespace {

using Recognition::Failure;
using Why = Recognition::Failure::Why;

constexpr auto converter = "ffmpeg"_L1;

[[nodiscard]] QString found(const QString &program) {
    if (QFileInfo(program).isAbsolute())
        return QFileInfo(program).isExecutable() ? program : QString();
    return QStandardPaths::findExecutable(program);
}

// Where a sentence ends, the segment does.
[[nodiscard]] bool endsSentence(const QString &word) { return word.endsWith(u'.') || word.endsWith(u'?') || word.endsWith(u'!'); }

} // namespace

WhisperLocal::WhisperLocal(QString program, QString modelFile, QObject *parent)
    : Recognizer(parent), m_program(std::move(program)), m_modelFile(std::move(modelFile)) {
    connect(&m_process, &QProcess::finished, this, &WhisperLocal::read);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            emit failed({.why = Why::Unsupported, .message = missing(m_converting ? QString(converter) : m_program, m_modelFile)});
    });
}

WhisperLocal::~WhisperLocal() { cancel(); }

QString WhisperLocal::findModel() {
    QStringList folders{QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/whisper.cpp/models"_s};
    for (const QString &data : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation))
        folders << data + u"/whisper.cpp/models"_s;
    for (const QString &folder : std::as_const(folders)) {
        const QStringList models = QDir(folder).entryList({u"ggml-*.bin"_s}, QDir::Files, QDir::Name);
        if (!models.isEmpty())
            return folder + u'/' + models.first();
    }
    return {};
}

QString WhisperLocal::missing(const QString &program, const QString &modelFile) {
    if (found(program).isEmpty())
        return tr("whisper.cpp isn't installed on this computer (there is no %1 command).").arg(program);
    if (modelFile.isEmpty() || !QFileInfo(modelFile).isFile())
        return tr("whisper.cpp has no model to transcribe with; put a ggml model in ~/.local/share/whisper.cpp/models.");
    return {};
}

QString WhisperLocal::where() const { return tr("whisper.cpp on this computer"); }

QString WhisperLocal::model() const { return QFileInfo(m_modelFile).completeBaseName(); }

void WhisperLocal::recognize(const Recognition::Input &input) {
    cancel();
    if (const QString why = missing(m_program, m_modelFile); !why.isEmpty()) {
        emit failed({.why = Why::Unsupported, .message = why});
        return;
    }
    m_folder = std::make_unique<QTemporaryDir>();
    m_language = input.language;
    if (input.file.endsWith(".wav"_L1, Qt::CaseInsensitive)) {
        transcribe(input.file);
        return;
    }
    // whisper.cpp reads 16 kHz WAV; anything else is made one first.
    if (found(converter).isEmpty()) {
        emit failed({.why = Why::Unsupported,
                     .message = tr("whisper.cpp reads only WAV recordings, and ffmpeg isn't installed to make one of %1.")
                                    .arg(QFileInfo(input.file).fileName())});
        return;
    }
    m_converting = true;
    m_process.start(converter, {u"-nostdin"_s, u"-loglevel"_s, u"error"_s, u"-y"_s, u"-i"_s, input.file, u"-ar"_s, u"16000"_s, u"-ac"_s,
                                u"1"_s, u"-c:a"_s, u"pcm_s16le"_s, m_folder->filePath(u"recording.wav"_s)});
}

void WhisperLocal::transcribe(const QString &wav) {
    m_converting = false;
    m_process.start(found(m_program), {u"-m"_s, m_modelFile, u"-f"_s, wav, u"-oj"_s, u"-of"_s, m_folder->filePath(u"transcript"_s),
                                       u"-ml"_s, u"1"_s, u"-sow"_s, u"-np"_s, u"-l"_s, m_language.isEmpty() ? u"auto"_s : m_language});
}

void WhisperLocal::read() {
    if (!m_folder)
        return; // cancelled
    const bool worked = m_process.exitStatus() == QProcess::NormalExit && m_process.exitCode() == 0;
    const QString said = QString::fromUtf8(m_process.readAllStandardError()).trimmed().section(u'\n', -1);
    if (!worked) {
        emit failed({.why = Why::Refused,
                     .message = m_converting ? tr("ffmpeg couldn't read the recording: %1").arg(said)
                                             : tr("whisper.cpp couldn't transcribe the recording: %1").arg(said)});
        return;
    }
    if (m_converting) {
        transcribe(m_folder->filePath(u"recording.wav"_s));
        return;
    }
    QFile output(m_folder->filePath(u"transcript.json"_s));
    if (!output.open(QIODevice::ReadOnly)) {
        emit failed({.why = Why::Refused, .message = tr("whisper.cpp wrote no transcript.")});
        return;
    }
    Recognition::Result result = Whisper::transcript(QJsonDocument::fromJson(output.readAll()).object());
    result.model = model();
    result.server = where();
    m_folder.reset();
    emit recognized(result);
}

void WhisperLocal::cancel() {
    m_folder.reset();
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(1000);
    }
}

Recognition::Result Whisper::transcript(const QJsonObject &output) {
    Recognition::Result result{.kind = Recognition::Kind::Audio,
                               .text = {},
                               .segments = {},
                               .language = output.value("result"_L1).toObject().value("language"_L1).toString(),
                               .model = {},
                               .server = {},
                               .at = QDateTime::currentMSecsSinceEpoch(),
                               .kept = false};
    Recognition::Segment open;
    const auto close = [&result, &open] {
        if (open.words.isEmpty())
            return;
        open.text = open.text.trimmed();
        open.end = open.words.last().end;
        result.segments.append(std::exchange(open, {}));
    };
    for (const auto &value : output.value("transcription"_L1).toArray()) {
        const QJsonObject entry = value.toObject();
        const QString text = entry.value("text"_L1).toString();
        const QJsonObject offsets = entry.value("offsets"_L1).toObject();
        if (text.trimmed().isEmpty())
            continue;
        if (open.words.isEmpty())
            open.start = offsets.value("from"_L1).toInteger();
        open.text += text;
        open.words.append(
            {.text = text.trimmed(), .start = offsets.value("from"_L1).toInteger(), .end = offsets.value("to"_L1).toInteger()});
        if (endsSentence(text.trimmed()))
            close();
    }
    close();
    QStringList texts;
    for (const Recognition::Segment &segment : std::as_const(result.segments))
        texts << segment.text;
    result.text = texts.join(u' ');
    return result;
}
