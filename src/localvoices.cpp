#include "localvoices.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <tuple>

using namespace Qt::StringLiterals;

namespace {

// A process given up on: it is killed and goes once it has ended, with
// nothing it says heard any more.
void abandon(QPointer<QProcess> &process) {
    if (!process)
        return;
    process->disconnect();
    QObject::connect(process, &QProcess::finished, process, &QObject::deleteLater);
    if (process->state() == QProcess::NotRunning)
        process->deleteLater();
    else
        process->kill();
    process = nullptr;
}

// What a program said when it failed, its last line, or its exit code.
[[nodiscard]] QString saidBy(QProcess &process, int exitCode) {
    const QStringList lines = QString::fromUtf8(process.readAllStandardError()).trimmed().split(u'\n', Qt::SkipEmptyParts);
    return lines.isEmpty() ? QString::number(exitCode) : lines.last().trimmed();
}

// Where Piper's voices are: the person's own, then the system's (where the
// AUR's piper-voices-* put them).
[[nodiscard]] QStringList voiceFolders() {
    QStringList folders;
    for (const QString &data : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation))
        folders << data + u"/piper-voices"_s << data + u"/piper"_s;
    folders << u"/usr/share/piper-voices"_s;
    folders.removeDuplicates();
    return folders;
}

} // namespace

SoundOut::SoundOut(QObject *parent) : QObject(parent) {
    m_player.setAudioOutput(&m_output);
    connect(&m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia)
            emit ended();
    });
    connect(&m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &why) { emit failed(why); });
}

void SoundOut::play(const QString &file) {
    m_player.stop();
    m_player.setSource(QUrl::fromLocalFile(file));
    m_player.play();
}

void SoundOut::stop() {
    m_player.stop();
    m_player.setSource({});
}

PiperVoice::PiperVoice(QString program, QStringList models, QObject *parent)
    : Voice(parent), m_program(std::move(program)), m_models(std::move(models)) {
    connect(&m_sound, &SoundOut::ended, this, &Voice::spoken);
    connect(&m_sound, &SoundOut::failed, this, [this](const QString &why) { emit failed(Failure(Failure::Code::VoiceUnplayed, {why})); });
}

PiperVoice::~PiperVoice() {
    drop(m_now);
    drop(m_next);
}

QString PiperVoice::findProgram() {
    for (const auto *name : {"piper-tts", "piper"})
        if (QString found = QStandardPaths::findExecutable(QString::fromLatin1(name)); !found.isEmpty())
            return found;
    return {};
}

QStringList PiperVoice::findModels() {
    QStringList models;
    QStringList names;
    for (const QString &folder : voiceFolders()) {
        QDirIterator files(folder, {u"*.onnx"_s}, QDir::Files, QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
        while (files.hasNext()) {
            const QString file = files.next();
            if (const QString name = QFileInfo(file).completeBaseName(); !names.contains(name)) {
                names << name;
                models << file;
            }
        }
    }
    std::ranges::sort(models, {}, [](const QString &file) { return QFileInfo(file).completeBaseName(); });
    return models;
}

QString PiperVoice::modelFor(const QStringList &models, const Speech &how) {
    const auto named = [&](const QString &file) { return QFileInfo(file).completeBaseName() == how.voice; };
    if (const auto chosen = std::ranges::find_if(models, named); !how.voice.isEmpty() && chosen != models.end())
        return *chosen;
    // Piper names a voice by its language first: "nl_BE-nathalie-medium".
    const QString language = how.language.left(2).toLower();
    const auto spoken = [&](const QString &file) { return QFileInfo(file).completeBaseName().left(2).toLower() == language; };
    if (const auto speaking = std::ranges::find_if(models, spoken); language.size() == 2 && speaking != models.end())
        return *speaking;
    return models.value(0);
}

QStringList PiperVoice::arguments(const QString &model, const Speech &how, const QString &wav) {
    // Slower is longer: Piper's length scale is the time a sound takes.
    const double scale = 1.0 / std::clamp(how.rate, 0.5, 2.0);
    return {u"-m"_s, model, u"-f"_s, wav, u"--length_scale"_s, QString::number(scale, 'f', 3)};
}

std::unique_ptr<PiperVoice::Making> PiperVoice::make(const QString &sentence, const Speech &how) {
    auto making = std::make_unique<Making>(Making{.sentence = sentence, .how = how, .wav = m_folder.filePath(u"%1.wav"_s.arg(++m_files))});
    auto *process = new QProcess(this);
    making->process = process;
    Making *raw = making.get();
    connect(process, &QProcess::finished, this, [this, raw](int exitCode, QProcess::ExitStatus) { made(raw, exitCode); });
    connect(process, &QProcess::errorOccurred, this, [this, raw](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            made(raw, -1);
    });
    process->start(m_program, arguments(modelFor(m_models, how), how, making->wav));
    process->write(sentence.toUtf8() + '\n');
    process->closeWriteChannel();
    return making;
}

void PiperVoice::made(Making *making, int exitCode) {
    QProcess *process = making->process;
    const bool written = exitCode == 0 && QFileInfo(making->wav).size() > 44;
    const QString said = process && !written ? saidBy(*process, exitCode) : QString();
    if (process)
        process->deleteLater();
    making->process = nullptr;
    making->made = written;
    if (making != m_now.get())
        return;
    if (!written) {
        emit failed(Failure(Failure::Code::VoiceStopped, {name(), said}));
        return;
    }
    m_sound.play(making->wav);
}

void PiperVoice::drop(std::unique_ptr<Making> &making) {
    if (!making)
        return;
    abandon(making->process);
    QFile::remove(making->wav);
    making.reset();
}

void PiperVoice::say(const QString &sentence, const Speech &how) {
    m_sound.stop();
    drop(m_now);
    if (m_next && m_next->sentence == sentence && m_next->how == how)
        m_now = std::move(m_next);
    else
        m_now = make(sentence, how);
    drop(m_next);
    if (m_now->made)
        m_sound.play(m_now->wav);
}

void PiperVoice::prepare(const QString &sentence, const Speech &how) {
    if (m_next && m_next->sentence == sentence && m_next->how == how)
        return;
    drop(m_next);
    m_next = make(sentence, how);
}

void PiperVoice::stop() {
    m_sound.stop();
    drop(m_now);
}

SpeechDispatcherVoice::SpeechDispatcherVoice(QString program, QObject *parent) : Voice(parent), m_program(std::move(program)) {}

SpeechDispatcherVoice::~SpeechDispatcherVoice() { drop(); }

QStringList SpeechDispatcherVoice::voices() {
    return {u"female1"_s, u"female2"_s, u"female3"_s, u"male1"_s, u"male2"_s, u"male3"_s, u"child_female"_s, u"child_male"_s};
}

QStringList SpeechDispatcherVoice::arguments(const QString &sentence, const Speech &how) {
    // Its rate is -100 to 100 around its own pace: half as fast is -100, twice as fast 100.
    const double rate = std::clamp(how.rate, 0.5, 2.0);
    const long shown = std::lround(rate >= 1 ? (rate - 1) * 100 : (rate - 1) * 200);
    QStringList arguments{u"-w"_s, u"-r"_s, QString::number(shown)};
    if (voices().contains(how.voice))
        arguments << u"-t"_s << how.voice;
    if (const QString language = how.language.left(2).toLower(); language.size() == 2)
        arguments << u"-l"_s << language;
    arguments << u"--"_s << sentence;
    return arguments;
}

void SpeechDispatcherVoice::say(const QString &sentence, const Speech &how) {
    drop();
    auto *process = new QProcess(this);
    m_process = process;
    connect(process, &QProcess::finished, this, [this, process](int exitCode, QProcess::ExitStatus status) {
        const QString said = exitCode != 0 || status != QProcess::NormalExit ? saidBy(*process, exitCode) : QString();
        process->deleteLater();
        m_process = nullptr;
        if (said.isEmpty())
            emit spoken();
        else
            emit failed(Failure(Failure::Code::VoiceStopped, {name(), said}));
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        process->deleteLater();
        m_process = nullptr;
        emit failed(Failure(Failure::Code::VoiceStopped, {name(), process->errorString()}));
    });
    process->start(m_program, arguments(sentence, how));
}

void SpeechDispatcherVoice::drop() {
    if (!m_process)
        return;
    abandon(m_process);
    // Its client gone, the service would read the sentence to its end.
    // Whether it could be stopped changes nothing for what is read next.
    std::ignore = QProcess::startDetached(m_program, {u"-S"_s});
}

void SpeechDispatcherVoice::stop() { drop(); }
