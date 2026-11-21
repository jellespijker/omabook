#include "bookwords.h"

#include <QSqlQuery>
#include <QVariant>
#include <algorithm>
#include <memory>
#include <optional>

#include "annotationwords.h"
#include "changelog.h"
#include "document.h"
#include "formats.h"

using namespace Qt::StringLiterals;

namespace {

// The books whose words hold every phrase: the index's rows are books.
[[nodiscard]] QString matching() { return u"SELECT rowid FROM book_words WHERE book_words MATCH ?"_s; }

} // namespace

QStringList BookWords::step() {
    return {u"CREATE TABLE IF NOT EXISTS book_texts (book_id INTEGER PRIMARY KEY REFERENCES books(id) ON DELETE CASCADE, "
            "hash TEXT NOT NULL)"_s,
            u"CREATE VIRTUAL TABLE IF NOT EXISTS book_words USING fts5(text, content='', contentless_delete=1, "
            "tokenize=\"unicode61 remove_diacritics 2 categories 'L* N* Co M*'\", prefix='2 3')"_s,
            // A book's words go with it, and with what was kept of it.
            u"CREATE TRIGGER IF NOT EXISTS book_texts_words_delete AFTER DELETE ON book_texts BEGIN "
            "DELETE FROM book_words WHERE rowid = OLD.book_id; END"_s};
}

QList<qint64> BookWords::stale(const Library &library, const QStringList &formats) {
    if (formats.isEmpty())
        return {};
    QVariantList values;
    for (const QString &format : formats)
        values << format.toLower();
    const QString marks = QStringList(formats.size(), u"?"_s).join(u", "_s);
    QList<qint64> found;
    for (QSqlQuery query =
             ChangeLog::query(library,
                              u"SELECT DISTINCT f.book_id FROM formats AS f LEFT JOIN book_texts AS t ON t.book_id = f.book_id "
                              "WHERE f.format IN (%1) AND (t.hash IS NULL OR t.hash <> f.hash) ORDER BY f.book_id"_s.arg(marks),
                              values);
         query.next();)
        found << query.value(0).toLongLong();
    return found;
}

std::expected<void, Failure> BookWords::index(const Library &library, qint64 bookId, const QStringList &formats) {
    const QList<FormatFile> files = library.formatFiles(bookId);
    const auto file = std::ranges::find_if(files, [&formats](const FormatFile &each) { return formats.contains(each.format); });
    if (file == files.end())
        return std::unexpected(Failure(Failure::Code::NoSuchBook, {QString::number(bookId)}));
    auto document = Formats::opened(file->path);
    if (!document)
        return std::unexpected(document.error());
    QStringList text;
    for (int section = 0; section < (*document)->count(); ++section)
        text << (*document)->text(section);
    return store(library, bookId, file->hash, text.join(u"\n\n"_s));
}

std::expected<void, Failure> BookWords::store(const Library &library, qint64 bookId, const QString &hash, const QString &text) {
    const Library::Batch batch(library, Library::Batch::Lock::AtOnce);
    const bool done = batch.begun() && ChangeLog::exec(library, u"DELETE FROM book_texts WHERE book_id = ?"_s, {bookId})
                      && ChangeLog::exec(library, u"INSERT INTO book_texts (book_id, hash) VALUES (?, ?)"_s, {bookId, hash})
                      && ChangeLog::exec(library, u"INSERT INTO book_words (rowid, text) VALUES (?, ?)"_s, {bookId, text});
    if (!done)
        return std::unexpected(Failure(Failure::Code::LibraryUnwritable, {u"book_words"_s}));
    return {};
}

QSet<qint64> BookWords::books(const Library &library, const QStringList &phrases) {
    const std::optional<QString> asked = AnnotationWords::query(phrases);
    QSet<qint64> found;
    if (!asked)
        return found;
    for (QSqlQuery query = ChangeLog::query(library, matching(), {*asked}); query.next();)
        found << query.value(0).toLongLong();
    return found;
}

QSet<qint64> BookWords::books(const QString &root, const QStringList &phrases) {
    const std::optional<QString> asked = AnnotationWords::query(phrases);
    return asked ? AnnotationWords::numbersIn(root, matching(), *asked) : QSet<qint64>();
}

Book::Noted BookWords::written(const Library &library) {
    return AnnotationWords::remembering([&library](const QString &words) { return books(library, {words}); });
}

Book::Noted BookWords::written(const QString &root) {
    return AnnotationWords::remembering([root](const QString &words) { return books(root, {words}); });
}
