#include "recognition.h"

#include <QCryptographicHash>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <ranges>

#include "failure.h"
#include "inkcodec.h"

using namespace Qt::StringLiterals;

namespace {

// Changed only when what a key is made of changes, so older keys are never
// taken for newer ones.
constexpr auto keyVersion = "omabook-recognition/1"_L1;

[[nodiscard]] QJsonArray wordsJson(const QList<Recognition::Word> &words) {
    QJsonArray list;
    for (const Recognition::Word &word : words)
        list.append(QJsonObject{{"text", word.text}, {"start", word.start}, {"end", word.end}});
    return list;
}

[[nodiscard]] QList<Recognition::Word> wordsOf(const QJsonArray &list) {
    QList<Recognition::Word> words;
    for (const auto &value : list) {
        const QJsonObject word = value.toObject();
        words.append({.text = word.value("text"_L1).toString(),
                      .start = word.value("start"_L1).toInteger(),
                      .end = word.value("end"_L1).toInteger()});
    }
    return words;
}

[[nodiscard]] QString hostOf(const QUrl &url) { return url.port() < 0 ? url.host() : u"%1:%2"_s.arg(url.host()).arg(url.port()); }

} // namespace

QString Recognition::kindName(Kind kind) { return kind == Kind::Ink ? u"ink"_s : u"audio"_s; }

std::optional<Recognition::Kind> Recognition::kindNamed(const QString &name) {
    if (name == "ink"_L1)
        return Kind::Ink;
    if (name == "audio"_L1)
        return Kind::Audio;
    return std::nullopt;
}

QJsonObject Recognition::Result::toJson() const {
    QJsonArray parts;
    for (const Segment &segment : segments)
        parts.append(
            QJsonObject{{"text", segment.text}, {"start", segment.start}, {"end", segment.end}, {"words", wordsJson(segment.words)}});
    return {{"kind", kindName(kind)}, {"text", text},   {"language", language}, {"model", model}, {"server", server}, {"at", at},
            {"kept", kept},           {"timed", timed}, {"segments", parts}};
}

std::optional<Recognition::Result> Recognition::Result::fromJson(const QJsonObject &json) {
    const auto kind = kindNamed(json.value("kind"_L1).toString());
    if (!kind)
        return std::nullopt;
    Result result{.kind = *kind,
                  .text = json.value("text"_L1).toString(),
                  .segments = {},
                  .language = json.value("language"_L1).toString(),
                  .model = json.value("model"_L1).toString(),
                  .server = json.value("server"_L1).toString(),
                  .at = json.value("at"_L1).toInteger(),
                  .kept = json.value("kept"_L1).toBool()};
    for (const auto &value : json.value("segments"_L1).toArray()) {
        const QJsonObject segment = value.toObject();
        result.segments.append({.text = segment.value("text"_L1).toString(),
                                .start = segment.value("start"_L1).toInteger(),
                                .end = segment.value("end"_L1).toInteger(),
                                .words = wordsOf(segment.value("words"_L1).toArray())});
    }
    // Kept before it was said: timed where any word has its moment.
    result.timed = json.contains("timed"_L1)
                       ? json.value("timed"_L1).toBool()
                       : std::ranges::any_of(result.segments, [](const Segment &one) { return !one.words.isEmpty(); });
    return result;
}

std::expected<QString, QString> Recognition::key(const Input &input, const QString &model) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QString(keyVersion + u'\n' + kindName(input.kind) + u'\n' + model + u'\n' + input.language + u'\n').toUtf8());
    if (input.kind == Kind::Ink) {
        if (std::ranges::all_of(input.strokes, [](const Ink::Stroke &stroke) { return stroke.points.empty(); }))
            return std::unexpected(::Failure(::Failure::Code::NoInk).text());
        // In the order they were written, whatever order they were kept in;
        // their ids are where they are kept, not what was written.
        std::vector<const Ink::Stroke *> written;
        written.reserve(input.strokes.size());
        for (const Ink::Stroke &stroke : input.strokes)
            written.push_back(&stroke);
        std::ranges::stable_sort(written, {}, [](const Ink::Stroke *stroke) { return stroke->began; });
        for (const Ink::Stroke *stroke : written) {
            const QByteArray bytes = Ink::encode(*stroke);
            hash.addData(QByteArray::number(bytes.size()) + ':' + bytes);
        }
        return QString::fromLatin1(hash.result().toHex());
    }
    QFile file(input.file);
    if (!file.open(QIODevice::ReadOnly))
        return std::unexpected(::Failure(::Failure::Code::RecordingUnreadable, {input.file}).text());
    if (file.size() == 0)
        return std::unexpected(::Failure(::Failure::Code::RecordingEmpty, {input.file}).text());
    if (!hash.addData(&file))
        return std::unexpected(::Failure(::Failure::Code::RecordingUnreadable, {input.file}).text());
    return QString::fromLatin1(hash.result().toHex());
}

void Recognition::Recognizer::resolve() { QTimer::singleShot(0, this, &Recognizer::resolved); }

Recognition::Reach Recognition::reach(const QString &baseUrl, const QStringList &own) {
    const QUrl url(baseUrl.trimmed());
    const QString host = url.host().toLower();
    QHostAddress address;
    const bool numeric = address.setAddress(host);
    if (host == "localhost"_L1 || (numeric && address.isLoopback()))
        return Reach::ThisComputer;
    // Tailscale's addresses, in IPv4 and IPv6, its names, and a name of one
    // word, which is the tailnet's own (MagicDNS).
    if (host.endsWith(".ts.net"_L1) || (!numeric && !host.isEmpty() && !host.contains('.'))
        || (numeric
            && (address.isInSubnet(QHostAddress::parseSubnet(u"100.64.0.0/10"_s))
                || address.isInSubnet(QHostAddress::parseSubnet(u"fd7a:115c:a1e0::/48"_s)))))
        return Reach::Tailnet;
    const QString here = hostOf(url).toLower();
    const bool marked = std::ranges::any_of(own, [&here, &host](const QString &mine) {
        const QUrl marked = QUrl::fromUserInput(mine.trimmed());
        return hostOf(marked).toLower() == here || (marked.port() < 0 && marked.host().toLower() == host);
    });
    return marked ? Reach::Own : Reach::Hosted;
}
