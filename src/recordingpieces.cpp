#include "recordingpieces.h"

#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include "opusreader.h"

using namespace Qt::StringLiterals;
using std::chrono::milliseconds;

namespace {

// Where the samples of a WAV of 16-bit samples are, and how they are laid out.
struct Wav {
    int channels = 1;
    int rate = 0;
    qint64 data = 0; // where the samples begin
    qint64 size = 0; // their bytes

    [[nodiscard]] qint64 frames() const { return size / (qint64{2} * channels); }
};

[[nodiscard]] quint32 le32(QByteArrayView bytes, qsizetype at) { return qFromLittleEndian<quint32>(bytes.sliced(at, 4).data()); }
[[nodiscard]] quint16 le16(QByteArrayView bytes, qsizetype at) { return qFromLittleEndian<quint16>(bytes.sliced(at, 2).data()); }

// The WAV's layout, when it is one of 16-bit samples Omabook reads itself.
[[nodiscard]] std::optional<Wav> wavOf(QFile &file) {
    const QByteArray head = file.read(12);
    if (head.size() < 12 || !head.startsWith("RIFF") || head.mid(8, 4) != "WAVE")
        return std::nullopt;
    Wav wav;
    bool pcm = false;
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(8);
        if (chunk.size() < 8)
            break;
        const qint64 size = le32(chunk, 4);
        const qint64 at = file.pos();
        if (chunk.startsWith("fmt ")) {
            const QByteArray format = file.read(16);
            if (format.size() < 16)
                return std::nullopt;
            constexpr quint16 integer = 1;
            constexpr quint16 extensible = 0xFFFE;
            pcm = (le16(format, 0) == integer || le16(format, 0) == extensible) && le16(format, 14) == 16;
            wav.channels = std::max<int>(1, le16(format, 2));
            wav.rate = static_cast<int>(le32(format, 4));
        } else if (chunk.startsWith("data")) {
            wav.data = at;
            // A recorder that never went back to write the size leaves it at its largest.
            wav.size = std::min(size, file.size() - at);
            return pcm && wav.rate > 0 ? std::optional(wav) : std::nullopt;
        }
        if (!file.seek(at + size + (size % 2)))
            break;
    }
    return std::nullopt;
}

// Samples of `channels` channels at `rate`, as one channel at 16 kHz: each
// sample made of those it stands for, put together (a box filter, which is
// enough for speech).
[[nodiscard]] QList<qint16> sixteenK(std::span<const qint16> samples, int channels, int rate) {
    const qint64 frames = static_cast<qint64>(samples.size()) / channels;
    const qint64 out = frames * RecordingPieces::sampleRate / rate;
    QList<qint16> made;
    made.reserve(out);
    for (qint64 index = 0; index < out; ++index) {
        const qint64 first = index * rate / RecordingPieces::sampleRate;
        const qint64 last = std::clamp<qint64>((index + 1) * rate / RecordingPieces::sampleRate, first + 1, frames);
        std::int64_t sum = 0;
        for (qint64 frame = first; frame < last; ++frame)
            for (int channel = 0; channel < channels; ++channel)
                sum += samples[static_cast<std::size_t>(frame * channels + channel)];
        made.append(static_cast<qint16>(sum / ((last - first) * channels)));
    }
    return made;
}

[[nodiscard]] std::expected<void, Failure> writeWav(const QString &path, const QList<qint16> &samples) {
    const auto bytes = static_cast<quint32>(samples.size() * 2);
    QByteArray wav("RIFF");
    const auto put32 = [&wav](quint32 value) {
        std::array<char, 4> four{};
        qToLittleEndian(value, four.data());
        wav.append(four.data(), 4);
    };
    const auto put16 = [&wav](quint16 value) {
        std::array<char, 2> two{};
        qToLittleEndian(value, two.data());
        wav.append(two.data(), 2);
    };
    put32(36 + bytes);
    wav.append("WAVEfmt ");
    put32(16);
    put16(1); // integer samples
    put16(1); // one channel
    put32(RecordingPieces::sampleRate);
    put32(RecordingPieces::sampleRate * 2);
    put16(2);
    put16(16);
    wav.append("data");
    put32(bytes);
    for (const qint16 sample : samples)
        put16(static_cast<quint16>(sample));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(wav) != wav.size() || !file.commit())
        return std::unexpected(Failure(Failure::Code::SoundFileUnwritten, {path, file.errorString()}));
    return {};
}

[[nodiscard]] bool isOgg(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && file.read(4) == "OggS";
}

[[nodiscard]] std::expected<QString, Failure> program(const QString &name, const QString &file) {
    QString found = QStandardPaths::findExecutable(name);
    if (found.isEmpty())
        return std::unexpected(Failure(Failure::Code::RecordingNeedsFfmpeg, {QFileInfo(file).fileName()}));
    return found;
}

// What a program printed, once it ended well; why not, said by ffmpeg, otherwise.
[[nodiscard]] std::expected<QByteArray, Failure> ran(const QString &command, const QStringList &arguments) {
    QProcess process;
    process.start(command, arguments);
    constexpr int tenMinutes = 10 * 60 * 1000;
    if (!process.waitForStarted() || !process.waitForFinished(tenMinutes) || process.exitStatus() != QProcess::NormalExit
        || process.exitCode() != 0)
        return std::unexpected(
            Failure(Failure::Code::FfmpegFailed, {QString::fromUtf8(process.readAllStandardError()).trimmed().left(300)}));
    return process.readAllStandardOutput();
}

[[nodiscard]] QString seconds(milliseconds moment) { return QString::number(static_cast<double>(moment.count()) / 1000.0, 'f', 3); }

} // namespace

std::expected<milliseconds, Failure> RecordingPieces::length(const QString &file) {
    QFile bytes(file);
    if (!bytes.open(QIODevice::ReadOnly))
        return std::unexpected(Failure(Failure::Code::RecordingUnreadable, {QFileInfo(file).fileName()}));
    if (const auto wav = wavOf(bytes))
        return milliseconds(wav->frames() * 1000 / wav->rate);
    if (isOgg(file))
        if (const auto opus = Opus::Reader::open(file))
            return std::chrono::duration_cast<milliseconds>((*opus)->duration());
    const auto ffprobe = program(u"ffprobe"_s, file);
    if (!ffprobe)
        return std::unexpected(ffprobe.error());
    const auto said = ran(
        *ffprobe, {u"-v"_s, u"error"_s, u"-show_entries"_s, u"format=duration"_s, u"-of"_s, u"default=noprint_wrappers=1:nokey=1"_s, file});
    if (!said)
        return std::unexpected(said.error());
    bool number = false;
    const double length = QString::fromUtf8(*said).trimmed().toDouble(&number);
    if (!number || length <= 0)
        return std::unexpected(Failure(Failure::Code::RecordingUnreadable, {QFileInfo(file).fileName()}));
    return milliseconds(static_cast<qint64>(length * 1000));
}

std::expected<void, Failure> RecordingPieces::cut(const QString &file, milliseconds from, milliseconds length, const QString &wav) {
    QFile bytes(file);
    if (!bytes.open(QIODevice::ReadOnly))
        return std::unexpected(Failure(Failure::Code::RecordingUnreadable, {QFileInfo(file).fileName()}));
    if (const auto layout = wavOf(bytes)) {
        const qint64 first = std::min<qint64>(from.count() * layout->rate / 1000, layout->frames());
        const qint64 frames = std::min<qint64>(length.count() * layout->rate / 1000, layout->frames() - first);
        const qint64 wanted = frames * layout->channels * 2;
        const QByteArray raw = bytes.seek(layout->data + first * layout->channels * 2) ? bytes.read(wanted) : QByteArray();
        if (raw.size() != wanted)
            return std::unexpected(Failure(Failure::Code::RecordingUnreadable, {QFileInfo(file).fileName()}));
        QList<qint16> samples(frames * layout->channels);
        for (qsizetype at = 0; at < samples.size(); ++at)
            samples[at] = qFromLittleEndian<qint16>(QByteArrayView(raw).sliced(at * 2, 2).data());
        return writeWav(wav, sixteenK({samples.constData(), static_cast<std::size_t>(samples.size())}, layout->channels, layout->rate));
    }
    if (isOgg(file))
        if (auto opus = Opus::Reader::open(file)) {
            const std::int64_t rate = Opus::sampleRate;
            (*opus)->seek(from.count() * rate / 1000);
            QList<qint16> samples(length.count() * rate / 1000);
            samples.resize((*opus)->read({samples.data(), static_cast<std::size_t>(samples.size())}));
            return writeWav(wav, sixteenK({samples.constData(), static_cast<std::size_t>(samples.size())}, 1, static_cast<int>(rate)));
        }
    const auto ffmpeg = program(u"ffmpeg"_s, file);
    if (!ffmpeg)
        return std::unexpected(ffmpeg.error());
    return ran(*ffmpeg, {u"-nostdin"_s, u"-v"_s, u"error"_s, u"-ss"_s, seconds(from), u"-t"_s, seconds(length), u"-i"_s, file, u"-ac"_s,
                         u"1"_s, u"-ar"_s, QString::number(sampleRate), u"-c:a"_s, u"pcm_s16le"_s, u"-f"_s, u"wav"_s, u"-y"_s, wav})
        .transform([](const QByteArray &) {});
}
