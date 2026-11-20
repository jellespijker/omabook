#include "annotationwords.h"

#include <QHash>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>
#include <functional>
#include <memory>
#include <ranges>
#include <utility>

#include "changelog.h"
#include "graphindex.h"

using namespace Qt::StringLiterals;

namespace {

// The annotations whose words the index holds every phrase of.
constexpr auto matching = "SELECT rowid FROM annotation_words WHERE annotation_words MATCH ?"_L1;

[[nodiscard]] QSet<qint64> numbersOf(QSqlQuery query) {
    QSet<qint64> numbers;
    while (query.next())
        numbers << query.value(0).toLongLong();
    return numbers;
}

[[nodiscard]] QString booksStatement() { return u"SELECT DISTINCT book_id FROM annotations WHERE id IN (%1)"_s.arg(matching); }

// A lookup that asks `books` once for each value, and remembers.
[[nodiscard]] Book::Noted remembering(std::function<QSet<qint64>(const QString &)> books) {
    const auto known = std::make_shared<QHash<QString, QSet<qint64>>>();
    return [books = std::move(books), known](const Book &book, const QString &words) {
        auto found = known->constFind(words);
        if (found == known->cend())
            found = known->insert(words, books(words));
        return found->contains(book.id);
    };
}

} // namespace

QStringList AnnotationWords::phrases(const QString &typed) {
    QStringList phrases;
    QString phrase;
    bool quoted = false;
    const auto close = [&phrases, &phrase] {
        if (!phrase.trimmed().isEmpty())
            phrases << phrase.simplified();
        phrase.clear();
    };
    for (const QChar c : typed) {
        if (c == u'"') {
            close();
            quoted = !quoted;
        } else if (c.isSpace() && !quoted) {
            close();
        } else {
            phrase += c;
        }
    }
    close();
    return phrases;
}

std::optional<QString> AnnotationWords::query(const QStringList &phrases) {
    const QStringList asked = phrases | std::views::transform(GraphIndex::phrase)
                              | std::views::filter([](const std::optional<QString> &phrase) { return phrase.has_value(); })
                              | std::views::transform([](const std::optional<QString> &phrase) { return *phrase; })
                              | std::ranges::to<QStringList>();
    return asked.isEmpty() ? std::nullopt : std::optional(asked.join(u" AND "_s));
}

QList<Annotation> AnnotationWords::in(const Library &library, qint64 bookId, const QStringList &phrases) {
    const std::optional<QString> asked = query(phrases);
    if (!asked)
        return {};
    const QSet<qint64> found = numbersOf(
        ChangeLog::query(library, u"SELECT id FROM annotations WHERE book_id = ? AND id IN (%1)"_s.arg(matching), {bookId, *asked}));
    return library.annotations(bookId)
           | std::views::filter([&found](const Annotation &annotation) { return found.contains(annotation.id); })
           | std::ranges::to<QList<Annotation>>();
}

QSet<qint64> AnnotationWords::books(const Library &library, const QStringList &phrases) {
    const std::optional<QString> asked = query(phrases);
    return asked ? numbersOf(ChangeLog::query(library, booksStatement(), {*asked})) : QSet<qint64>();
}

QSet<qint64> AnnotationWords::books(const QString &root, const QStringList &phrases) {
    const std::optional<QString> asked = query(phrases);
    if (!asked || root.isEmpty())
        return {};
    const QString connection = u"omabook-words-"_s + QUuid::createUuid().toString(QUuid::Id128);
    QSet<qint64> found;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(u"QSQLITE"_s, connection);
        database.setDatabaseName(Library::databaseIn(root));
        database.setConnectOptions(u"QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=2000"_s);
        if (database.open()) {
            QSqlQuery query(database);
            query.prepare(booksStatement());
            query.addBindValue(*asked);
            if (query.exec())
                found = numbersOf(std::move(query));
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return found;
}

Book::Noted AnnotationWords::noted(const Library &library) {
    return remembering([&library](const QString &words) { return books(library, {words}); });
}

Book::Noted AnnotationWords::noted(const QString &root) {
    return remembering([root](const QString &words) { return books(root, {words}); });
}
