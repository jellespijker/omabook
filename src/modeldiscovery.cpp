#include "modeldiscovery.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonValue>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace {

constexpr qint64 megabyte = 1024LL * 1024;
// Q4_K_M, Ollama's default quantisation, keeps some 4.85 bits of each weight.
constexpr double q4BitsPerWeight = 4.85;

[[nodiscard]] QString decoded(QString text) {
    text.replace("&amp;"_L1, "&"_L1).replace("&#39;"_L1, "'"_L1).replace("&quot;"_L1, "\""_L1);
    return text.replace("&lt;"_L1, "<"_L1).replace("&gt;"_L1, ">"_L1).simplified();
}

// Every text an element marked with this attribute holds: Ollama's page
// marks the parts of a result for its own tests (x-test-size, ...).
[[nodiscard]] QStringList marked(const QString &html, QLatin1StringView attribute) {
    QStringList found;
    const QRegularExpression pattern(u"<[^>]*\\b"_s + attribute + u"\\b[^>]*>([^<]*)<"_s);
    for (auto match = pattern.globalMatch(html); match.hasNext();)
        found << decoded(match.next().captured(1));
    found.removeAll(QString());
    return found;
}

// "1.2M", "56K", "312": a pull count as the page says it.
[[nodiscard]] qint64 countOf(const QString &said) {
    QString number = said.trimmed().toUpper();
    double times = 1;
    if (number.endsWith(u'K'))
        times = 1e3;
    else if (number.endsWith(u'M'))
        times = 1e6;
    else if (number.endsWith(u'B'))
        times = 1e9;
    if (times > 1)
        number.chop(1);
    return std::llround(number.toDouble() * times);
}

[[nodiscard]] QString licenceOf(const QJsonArray &tags) {
    for (const QJsonValue tag : tags)
        if (const QString word = tag.toString(); word.startsWith("license:"_L1))
            return word.mid(8);
    return {};
}

[[nodiscard]] qint64 sizeOf(const QJsonObject &entry) {
    const qint64 size = entry.value("size"_L1).toInteger();
    return size > 0 ? size : entry.value("lfs"_L1).toObject().value("size"_L1).toInteger();
}

} // namespace

QVariantMap Discovery::Found::toMap() const {
    const auto fitName = [this] {
        switch (fit) {
        case Fit::Gpu:
            return u"gpu"_s;
        case Fit::Memory:
            return u"memory"_s;
        case Fit::TooBig:
            return u"too-big"_s;
        case Fit::Unknown:
            break;
        }
        return u"unknown"_s;
    };
    const bool hub = source == Source::HuggingFace;
    const QString page = hub ? u"https://huggingface.co/"_s + repository : u"https://ollama.com/library/"_s + repository;
    return {{u"source"_s, hub ? u"hugging-face"_s : u"ollama"_s},
            {u"name"_s, name},
            {u"repository"_s, repository},
            {u"page"_s, page},
            {u"description"_s, description},
            {u"licence"_s, licence},
            {u"bytes"_s, bytes},
            {u"needs"_s, needs},
            {u"downloads"_s, downloads},
            {u"likes"_s, likes},
            {u"hosted"_s, hosted},
            {u"fit"_s, fitName()}};
}

QString Discovery::hubTask(Capability capability) {
    switch (capability) {
    case Capability::Handwriting:
        return u"image-text-to-text"_s;
    case Capability::Embeddings:
        return u"feature-extraction"_s;
    case Capability::SpeechToText:
        return u"automatic-speech-recognition"_s;
    case Capability::TextToSpeech:
        return u"text-to-speech"_s;
    case Capability::Answers:
    case Capability::Quick:
        break;
    }
    return u"text-generation"_s;
}

QString Discovery::libraryCategory(Capability capability) {
    if (capability == Capability::Handwriting)
        return u"vision"_s;
    return capability == Capability::Embeddings ? u"embedding"_s : QString();
}

bool Discovery::pulled(Capability capability) { return capability != Capability::SpeechToText && capability != Capability::TextToSpeech; }

QString Discovery::speechRepository(Capability capability) {
    if (capability == Capability::SpeechToText)
        return u"ggerganov/whisper.cpp"_s;
    return capability == Capability::TextToSpeech ? u"rhasspy/piper-voices"_s : QString();
}

QList<Discovery::Found> Discovery::fromLibrary(const QString &html, qsizetype most) {
    QList<Found> found;
    QStringList results = html.split(u"x-test-model"_s);
    if (!results.isEmpty())
        results.removeFirst(); // what comes before the first
    static const QRegularExpression link(u"href=\"/([^\"]+)\""_s);
    static const QRegularExpression said(u"<p[^>]*>([^<]+)</p>"_s);
    for (const QString &result : std::as_const(results)) {
        const QStringList titles = marked(result, "x-test-search-response-title"_L1);
        const auto linked = link.match(result);
        if (titles.isEmpty() && !linked.hasMatch())
            continue;
        QString repository = linked.hasMatch() ? linked.captured(1) : titles.first();
        if (repository.startsWith("library/"_L1))
            repository = repository.mid(8);
        const QString description = decoded(said.match(result).captured(1));
        const QStringList abilities = marked(result, "x-test-capability"_L1);
        QStringList sizes = marked(result, "x-test-size"_L1);
        const qint64 pulls = countOf(marked(result, "x-test-pull-count"_L1).value(0));
        const bool cloud = abilities.contains("cloud"_L1) || sizes.contains("cloud"_L1);
        // Ollama's own computers: one result, by its cloud tag.
        if (cloud)
            sizes = {u"cloud"_s};
        if (sizes.isEmpty())
            sizes = {QString()};
        for (const QString &size : std::as_const(sizes)) {
            Found one;
            one.source = Source::Ollama;
            one.repository = repository;
            one.name = size.isEmpty() ? repository : repository + u':' + size;
            one.description = description;
            one.bytes = cloud ? 0 : bytesOfParameters(size);
            one.downloads = pulls;
            one.hosted = cloud;
            found << one;
            if (found.size() >= most)
                return found;
        }
    }
    return found;
}

Discovery::Manifest Discovery::fromManifest(const QJsonObject &manifest) {
    Manifest made;
    made.bytes = manifest.value("config"_L1).toObject().value("size"_L1).toInteger();
    for (const QJsonValue layer : manifest.value("layers"_L1).toArray()) {
        const QJsonObject one = layer.toObject();
        made.bytes += one.value("size"_L1).toInteger();
        if (one.value("mediaType"_L1).toString() == "application/vnd.ollama.image.license"_L1 && made.licence.isEmpty())
            made.licence = one.value("digest"_L1).toString();
    }
    return made;
}

QString Discovery::licenceName(const QString &text) {
    for (const QString &line : text.split(u'\n')) {
        if (const QString words = line.simplified(); !words.isEmpty())
            return words.left(80);
    }
    return {};
}

QList<Discovery::Found> Discovery::fromHub(const QJsonArray &models) {
    QList<Found> found;
    for (const QJsonValue model : models) {
        const QJsonObject one = model.toObject();
        const QString id = one.value("id"_L1).toString(one.value("modelId"_L1).toString());
        if (id.isEmpty())
            continue;
        Found each;
        each.source = Source::HuggingFace;
        each.repository = id;
        each.name = u"hf.co/"_s + id;
        each.licence = licenceOf(one.value("tags"_L1).toArray());
        each.downloads = one.value("downloads"_L1).toInteger();
        each.likes = one.value("likes"_L1).toInteger();
        found << each;
    }
    return found;
}

std::optional<Discovery::Gguf> Discovery::ggufOf(const QJsonArray &tree) {
    static const QRegularExpression quantised(u"[._-](I?Q\\d[A-Z0-9_]*|BF16|F16|F32)\\.gguf$"_s, QRegularExpression::CaseInsensitiveOption);
    std::optional<Gguf> chosen;
    qint64 projector = 0; // a vision model's mmproj, which Ollama pulls along
    for (const QJsonValue entry : tree) {
        const QJsonObject file = entry.toObject();
        const QString path = file.value("path"_L1).toString();
        if (file.value("type"_L1).toString() != "file"_L1 || !path.endsWith(".gguf"_L1, Qt::CaseInsensitive))
            continue;
        if (QFileInfo(path).fileName().contains("mmproj"_L1, Qt::CaseInsensitive)) {
            projector = projector == 0 ? sizeOf(file) : std::min(projector, sizeOf(file));
            continue;
        }
        const auto match = quantised.match(path);
        const Gguf one{.quant = match.hasMatch() ? match.captured(1).toUpper() : QString(), .bytes = sizeOf(file)};
        const bool preferred = one.quant == "Q4_K_M"_L1;
        if (!chosen || preferred || (chosen->quant != "Q4_K_M"_L1 && one.bytes < chosen->bytes))
            chosen = one;
    }
    if (chosen)
        chosen->bytes += projector;
    return chosen;
}

QList<Discovery::Found> Discovery::speechFiles(Capability capability, const QJsonArray &tree, const QString &terms, qsizetype most) {
    QList<Found> found;
    const QStringList words = terms.toLower().split(u' ', Qt::SkipEmptyParts);
    for (const QJsonValue entry : tree) {
        const QJsonObject file = entry.toObject();
        const QString path = file.value("path"_L1).toString();
        const QString name = QFileInfo(path).fileName();
        const bool whisper = capability == Capability::SpeechToText && name.startsWith("ggml-"_L1) && name.endsWith(".bin"_L1);
        const bool piper = capability == Capability::TextToSpeech && name.endsWith(".onnx"_L1);
        if (file.value("type"_L1).toString() != "file"_L1 || (!whisper && !piper))
            continue;
        if (!std::ranges::all_of(words, [&path](const QString &word) { return path.contains(word, Qt::CaseInsensitive); }))
            continue;
        Found one;
        one.source = Source::HuggingFace;
        one.repository = speechRepository(capability);
        one.file = path;
        one.name = piper ? QFileInfo(name).completeBaseName() : name;
        one.bytes = sizeOf(file);
        found << one;
        if (found.size() >= most)
            break;
    }
    return found;
}

qint64 Discovery::bytesOfParameters(const QString &size) {
    static const QRegularExpression parameters(u"^(\\d+(?:\\.\\d+)?)([bmk])$"_s, QRegularExpression::CaseInsensitiveOption);
    // "8x7b" and the like: its experts together.
    QString plain = size.trimmed().toLower();
    double experts = 1;
    if (const qsizetype times = plain.indexOf(u'x'); times > 0) {
        experts = plain.left(times).toDouble();
        plain = plain.mid(times + 1);
    }
    const auto match = parameters.match(plain);
    if (!match.hasMatch() || experts <= 0)
        return 0;
    const QChar unit = match.captured(2).at(0);
    double scale = 1e3;
    if (unit == u'b')
        scale = 1e9;
    else if (unit == u'm')
        scale = 1e6;
    return std::llround(experts * match.captured(1).toDouble() * scale * q4BitsPerWeight / 8.0);
}

qint64 Discovery::needs(Capability capability, qint64 bytes) {
    if (bytes <= 0)
        return 0;
    switch (capability) {
    case Capability::Embeddings:
        return bytes + bytes / 10 + 256 * megabyte;
    case Capability::SpeechToText:
        return bytes + bytes / 3 + 200 * megabyte;
    case Capability::TextToSpeech:
        return bytes + bytes / 2 + 100 * megabyte;
    case Capability::Answers:
    case Capability::Quick:
    case Capability::Handwriting:
        break;
    }
    // The weights, and a context's worth of room for what is asked.
    return bytes + bytes / 5 + 512 * megabyte;
}

Discovery::Free Discovery::here(const QString &root) {
    Free free;
    const QDir base(root);
    QFile info(base.filePath(u"proc/meminfo"_s));
    if (info.open(QIODevice::ReadOnly)) {
        for (const QByteArray &line : info.readAll().split('\n')) {
            if (!line.startsWith("MemAvailable:"))
                continue;
            free.memory = line.mid(13).trimmed().split(' ').value(0).toLongLong() * 1024;
            break;
        }
    }
    const QDir cards(base.filePath(u"sys/class/drm"_s));
    for (const QString &card : cards.entryList({u"card*"_s}, QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (card.contains(u'-'))
            continue; // a card's connector, not a card
        const auto read = [&cards, &card](const QString &name) {
            QFile file(cards.filePath(card + u"/device/"_s + name));
            return file.open(QIODevice::ReadOnly) ? file.readAll().trimmed().toLongLong() : 0LL;
        };
        free.gpu = std::max(free.gpu, read(u"mem_info_vram_total"_s) - read(u"mem_info_vram_used"_s));
    }
    free.known = free.memory > 0;
    return free;
}

Discovery::Fit Discovery::fit(qint64 needs, const Free &free) {
    if (!free.known || needs <= 0)
        return Fit::Unknown;
    if (free.gpu >= needs)
        return Fit::Gpu;
    return free.memory >= needs ? Fit::Memory : Fit::TooBig;
}
