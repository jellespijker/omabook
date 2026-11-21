#include "koreaderbooks.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSysInfo>
#include <algorithm>
#include <tuple>

#include "document.h"
#include "formats.h"
#include "koreaderdocument.h"

using namespace Qt::StringLiterals;

QJsonObject KoreaderRecord::toJson() const {
    return {{"document", document}, {"progress", progress},  {"percentage", percentage},
            {"device", device},     {"device_id", deviceId}, {"timestamp", timestamp}};
}

std::optional<KoreaderRecord> KoreaderRecord::fromJson(const QJsonObject &object) {
    KoreaderRecord record;
    record.document = object.value("document").toString();
    // A page number comes as a number from some versions, as text from others.
    const QJsonValue progress = object.value("progress");
    record.progress = progress.isDouble() ? QString::number(progress.toInteger()) : progress.toString();
    record.percentage = std::clamp(object.value("percentage").toDouble(), 0.0, 1.0);
    record.device = object.value("device").toString();
    record.deviceId = object.value("device_id").toString();
    record.timestamp = object.value("timestamp").toInteger();
    if (record.document.isEmpty() || record.progress.isEmpty())
        return std::nullopt;
    return record;
}

KoreaderBooks::KoreaderBooks(Library &library) : m_library(&library) {}

QString KoreaderBooks::deviceId(const Library &library) {
    const QByteArray named = QSysInfo::machineUniqueId() + library.root().toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(named, QCryptographicHash::Md5).toHex().toUpper());
}

void KoreaderBooks::index() {
    const Library::Changes now = m_library->changes();
    if (m_indexed == now)
        return;
    m_indexed = now;
    m_byDocument.clear();
    for (const Book &book : m_library->books()) {
        for (const FormatFile &file : m_library->formatFiles(book.id)) {
            // A file is hashed once, whatever is done to the library after.
            QString document = m_documentOfFile.value(file.hash);
            if (document.isEmpty()) {
                document = KoreaderDocument::hash(file.path);
                m_documentOfFile.insert(file.hash, document);
            }
            if (!document.isEmpty())
                m_byDocument.insert(document, {.book = book.id, .format = file.format, .path = file.path});
        }
    }
}

std::optional<KoreaderBooks::File> KoreaderBooks::find(const QString &document) {
    index();
    const auto found = m_byDocument.constFind(document.toLower());
    if (found == m_byDocument.cend())
        return std::nullopt;
    return *found;
}

QList<std::pair<QString, KoreaderBooks::File>> KoreaderBooks::filesOf(qint64 book) {
    index();
    QList<std::pair<QString, File>> files;
    for (auto it = m_byDocument.cbegin(); it != m_byDocument.cend(); ++it) {
        if (it->book == book)
            files.append({it.key(), *it});
    }
    return files;
}

std::vector<qsizetype> KoreaderBooks::sectionSizes(const QString &path) {
    const auto kept = m_sizes.constFind(path);
    if (kept != m_sizes.cend())
        return *kept;
    std::vector<qsizetype> sizes;
    if (const auto document = Formats::opened(path)) {
        const Document &opened = **document;
        sizes.reserve(static_cast<std::size_t>(opened.count()));
        for (int section = 0; section < opened.count(); ++section)
            sizes.push_back(opened.kind() == Document::Pages ? 1 : opened.text(section).size());
    }
    m_sizes.insert(path, sizes);
    return sizes;
}

std::optional<KoreaderRecord> KoreaderBooks::recordOf(const File &file, const QString &document, const QString &deviceId) {
    const auto book = m_library->book(file.book);
    const Progress progress = m_library->progress(file.book);
    if (!book || progress.updated <= 0)
        return std::nullopt;
    const double percentage = std::clamp(progress.percent / 100, 0.0, 1.0);
    // The place is in the terms of the format the book is read in here; in
    // another, only how far through it is known.
    const BookPlace place = file.format == Formats::openedAs(book->formats)
                                ? progress.place
                                : KoreaderDocument::placeOf({}, percentage, file.format, sectionSizes(file.path));
    const auto pointer = KoreaderDocument::progressOf(place, file.format);
    if (!pointer)
        return std::nullopt;
    return KoreaderRecord{.document = document,
                          .progress = *pointer,
                          .percentage = percentage,
                          .device = u"Omabook"_s,
                          .deviceId = deviceId,
                          .timestamp = progress.updated};
}

bool KoreaderBooks::take(const File &file, const KoreaderRecord &record) {
    const auto book = m_library->book(file.book);
    if (!book || record.timestamp <= m_library->progress(file.book).updated)
        return false;
    const QString readAs = Formats::openedAs(book->formats);
    const BookPlace place =
        readAs == file.format
            ? KoreaderDocument::placeOf(record.progress, record.percentage, file.format, sectionSizes(file.path))
            : KoreaderDocument::placeOf({}, record.percentage, readAs, sectionSizes(m_library->formatPath(file.book, readAs)));
    m_library->setProgress(file.book, {.place = place, .percent = record.percentage * 100, .updated = record.timestamp});
    return true;
}

namespace {

[[nodiscard]] QString recordsFile(const Library &library) { return QFileInfo(library.databasePath()).dir().filePath(u"koreader.json"_s); }

[[nodiscard]] QJsonObject allHeard(const Library &library) {
    QFile file(recordsFile(library));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

} // namespace

std::optional<KoreaderRecord> KoreaderRecords::heard(const Library &library, const QString &document) {
    return KoreaderRecord::fromJson(allHeard(library).value(document).toObject());
}

void KoreaderRecords::hear(const Library &library, const KoreaderRecord &record) {
    QJsonObject all = allHeard(library);
    all.insert(record.document, record.toJson());
    QSaveFile file(recordsFile(library));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(all).toJson(QJsonDocument::Compact));
        std::ignore = file.commit(); // A record not kept is asked for again; nothing is lost.
    }
}
