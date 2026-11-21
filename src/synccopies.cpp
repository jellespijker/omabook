#include "synccopies.h"

#include <QJsonArray>
#include <algorithm>
#include <tuple>
#include <utility>

#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] std::pair<bool, qint64> speakingOrder(const Book &book) { return {book.formats.isEmpty(), book.id}; }

[[nodiscard]] std::pair<bool, qint64> speakingOrder(const QJsonObject &book) {
    return {book.value("files").toArray().isEmpty(), book.value("id").toInteger()};
}

} // namespace

QList<QList<Book>> SyncCopies::of(const Library &library, const QList<Book> &books, const QHash<QString, QString> &canonical) {
    QHash<QString, qsizetype> at;
    QList<QList<Book>> copies;
    for (const Book &book : books) {
        const QString gid = Spaces::bookId(library, book.id);
        const QString key = gid.isEmpty() ? u"#"_s + QString::number(book.id) : canonical.value(gid, gid);
        if (const auto found = at.constFind(key); found != at.cend()) {
            copies[*found] << book;
            continue;
        }
        at.insert(key, copies.size());
        copies << QList<Book>{book};
    }
    for (QList<Book> &copy : copies)
        std::ranges::sort(copy, {}, [](const Book &book) { return speakingOrder(book); });
    return copies;
}

bool SyncCopies::speaksBefore(const QJsonObject &one, const QJsonObject &other) { return speakingOrder(one) < speakingOrder(other); }

bool SyncCopies::sameWords(const BookMeta &one, const BookMeta &other) {
    return std::tie(one.title, one.authors, one.series, one.seriesIndex, one.publisher, one.language, one.isbn, one.published,
                    one.description, one.tags)
           == std::tie(other.title, other.authors, other.series, other.seriesIndex, other.publisher, other.language, other.isbn,
                       other.published, other.description, other.tags);
}
