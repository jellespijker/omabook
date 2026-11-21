#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QtTypes>
#include <optional>
#include <vector>

#include "library.h"

// A reading place as KOReader's progress sync says it (#71): the document
// (KoreaderDocument::hash), where in it (a page number or an XPointer), how
// far into the whole of it from 0 to 1, the device that said it, and when,
// in seconds since the epoch.
struct KoreaderRecord {
    QString document;
    QString progress;
    double percentage = 0;
    QString device;
    QString deviceId;
    qint64 timestamp = 0;

    // As the protocol writes it: {"document", "progress", "percentage", "device", "device_id", "timestamp"}.
    [[nodiscard]] QJsonObject toJson() const;
    // None without a document or a progress.
    [[nodiscard]] static std::optional<KoreaderRecord> fromJson(const QJsonObject &object);
};

// The books of a library as KOReader knows them: each of a book's files by
// the hash KOReader names it by, and its reading place in KOReader's terms
// both ways. What is worked out (a file's hash, its sections' sizes) is kept
// for as long as this lives, and the hashes looked at again when the
// library's books change. One thread, as the Library it reads.
class KoreaderBooks {
public:
    explicit KoreaderBooks(Library &library);

    // What this computer and library are called to KOReader, as a device of its own: the same every time.
    [[nodiscard]] static QString deviceId(const Library &library);

    // A book's file that KOReader has.
    struct File {
        qint64 book = 0;
        QString format;
        QString path;
    };
    // The file KOReader names by this document hash; none for one that isn't here.
    [[nodiscard]] std::optional<File> find(const QString &document);
    // The hash of every file of a book, each with it.
    [[nodiscard]] QList<std::pair<QString, File>> filesOf(qint64 book);

    // What KOReader is told of the place a book was read to, as of the file
    // it has: none when it wasn't read here, or its format can't be pointed
    // into. `deviceId` names this computer to KOReader.
    [[nodiscard]] std::optional<KoreaderRecord> recordOf(const File &file, const QString &document, const QString &deviceId);
    // Takes KOReader's place into the library, when it is newer than the
    // book's place here; whether it did.
    bool take(const File &file, const KoreaderRecord &record);

    // How long each section's text is (as many 1s as a paged book has pages).
    [[nodiscard]] std::vector<qsizetype> sectionSizes(const QString &path);

private:
    void index();

    Library *m_library; // never null: what it was made with
    std::optional<Library::Changes> m_indexed;
    QHash<QString, File> m_byDocument;
    QHash<QString, QString> m_documentOfFile; // a file's SHA-256 → its KOReader hash
    QHash<QString, std::vector<qsizetype>> m_sizes;
};

// The last place each KOReader device said of a document, kept beside the
// library's database (koreader.json), so a device that asks is answered in
// its own terms, and knows the place as its own, when nothing newer was read
// here since.
namespace KoreaderRecords {

[[nodiscard]] std::optional<KoreaderRecord> heard(const Library &library, const QString &document);
void hear(const Library &library, const KoreaderRecord &record);

} // namespace KoreaderRecords
