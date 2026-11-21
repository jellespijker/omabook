#include "booklinks.h"

#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <algorithm>
#include <utility>

#include "bookmeta.h"
#include "library.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] bool isBookLink(const SpaceOperation &op) { return op.entity == "book"_L1 && op.field == "same_as"_L1; }

// Of each book's id, the fields that name it, each as it was written last.
using Naming = QHash<QString, QHash<QString, std::pair<qint64, QString>>>;

[[nodiscard]] Naming namingOf(std::span<const SpaceOperation> ops) {
    static const QSet<QString> naming{u"title"_s, u"authors"_s, u"isbn"_s, u"identifiers"_s};
    Naming books;
    for (const SpaceOperation &op : ops) {
        if (op.entity != "book"_L1 || !naming.contains(op.field))
            continue;
        auto &field = books[op.id][op.field];
        if (op.hlc >= field.first)
            field = {op.hlc, op.value};
    }
    return books;
}

// The entry keys of a book's id as the log names it now; none when it says nothing of it.
[[nodiscard]] QSet<QString> keysOf(const Naming &books, const QString &id) {
    const auto book = books.constFind(id);
    if (book == books.cend())
        return {};
    const auto text = [&book](const QString &field) { return book->value(field).second; };
    const QStringList keys = Library::entryKeys(QJsonDocument::fromJson(text(u"identifiers"_s).toUtf8()).object(), text(u"isbn"_s),
                                                text(u"title"_s), BookMeta::splitAuthors(text(u"authors"_s)));
    return {keys.cbegin(), keys.cend()};
}

[[nodiscard]] bool holds(const Naming &books, const SpaceOperation &link) {
    if (link.id.startsWith("b:"_L1) && link.value.startsWith("b:"_L1))
        return true;
    const QSet<QString> one = keysOf(books, link.id);
    const QSet<QString> other = keysOf(books, link.value);
    return !books.contains(link.id) || !books.contains(link.value) || one.intersects(other);
}

} // namespace

QList<SpaceOperation> BookLinks::trusted(std::span<const SpaceOperation> ops) {
    const Naming books = namingOf(ops);
    QList<SpaceOperation> kept;
    kept.reserve(static_cast<qsizetype>(ops.size()));
    std::ranges::copy_if(ops, std::back_inserter(kept), [&books](const SpaceOperation &op) { return !isBookLink(op) || holds(books, op); });
    return kept;
}
