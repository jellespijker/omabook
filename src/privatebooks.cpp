#include "privatebooks.h"

#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QVariant>
#include <algorithm>
#include <optional>

#include "changelog.h"
#include "formats.h"
#include "library.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

constexpr auto entity = "private"_L1;
constexpr auto kindPrefix = "kind:"_L1;
constexpr auto pluginField = "plugin"_L1;
constexpr auto sharedField = "shared"_L1;

// Of each thing and field of the entity, the latest word, a tie going to the
// larger device, as of any field (SpaceReducer).
[[nodiscard]] QHash<QString, QHash<QString, QString>> said(const Library &library) {
    QHash<QString, QHash<QString, QString>> latest;
    for (QSqlQuery query = ChangeLog::query(library,
                                            "SELECT i.name, o.field, o.value FROM live_ops AS o JOIN log_ids AS d ON d.id = o.device "
                                            "JOIN log_ids AS i ON i.id = o.id WHERE o.entity = ? ORDER BY o.hlc, d.name",
                                            {QString(entity)});
         query.next();)
        latest[query.value(0).toString()].insert(query.value(1).toString(), query.value(2).toString());
    return latest;
}

[[nodiscard]] bool isFormatName(const QString &format) {
    static const QRegularExpression name(u"^[a-z0-9][a-z0-9.]{0,15}$"_s);
    return name.match(format).hasMatch();
}

[[nodiscard]] bool sharedIn(const QHash<QString, QHash<QString, QString>> &all, const Library &library, qint64 book) {
    return all.value(Spaces::bookId(library, book)).value(QString(sharedField)) == "1"_L1;
}

[[nodiscard]] std::optional<PrivateBooks::Kind> kindOf(const QList<PrivateBooks::Kind> &kinds, const Book &book) {
    for (const PrivateBooks::Kind &kind : kinds)
        if (book.formats.contains(kind.format))
            return kind;
    return std::nullopt;
}

// Whether the collection, or one inside it, holds the book by a choice by hand.
[[nodiscard]] bool byHand(const Collection &collection, qint64 book) {
    return collection.included.contains(book)
           || std::ranges::any_of(collection.within, [book](const Collection::Part &part) { return part.included.contains(book); });
}

[[nodiscard]] std::expected<void, Failure> append(const Library &library, const QString &id, const QString &field, const QString &value) {
    // At once: the log is read for the next number before the operation is
    // written (ReadingGoal::set says why).
    const Library::Batch batch(library, Library::Batch::Lock::AtOnce);
    if (!batch.begun())
        return std::unexpected(Failure(Failure::Code::LibraryBusy));
    ChangeLog::append(library, {Spaces::personalSpace(library), QString(entity), id, field, value, std::nullopt});
    return {};
}

} // namespace

QList<PrivateBooks::Kind> PrivateBooks::kinds(const Library &library) {
    QList<Kind> kinds;
    const auto all = said(library);
    for (auto thing = all.cbegin(); thing != all.cend(); ++thing) {
        if (!thing.key().startsWith(kindPrefix) || !thing->contains(QString(pluginField)))
            continue;
        const QJsonObject plugin = QJsonDocument::fromJson(thing->value(QString(pluginField)).toUtf8()).object();
        kinds << Kind{.format = thing.key().mid(kindPrefix.size()),
                      .plugin = plugin.value("plugin"_L1).toString(),
                      .title = plugin.value("title"_L1).toString()};
    }
    std::ranges::sort(kinds, {}, &Kind::format);
    return kinds;
}

std::expected<bool, Failure> PrivateBooks::declare(const Library &library, const Kind &kind) {
    const QString format = kind.format.trimmed().toLower();
    if (!isFormatName(format) || Formats::ownFormat(format) || kind.plugin.isEmpty())
        return false;
    const QList<Kind> known = kinds(library);
    if (std::ranges::any_of(known, [&format](const Kind &one) { return one.format == format; }))
        return false;
    const QJsonObject plugin{{"plugin"_L1, kind.plugin}, {"title"_L1, kind.title.isEmpty() ? kind.plugin : kind.title}};
    const auto written =
        append(library, kindPrefix + format, QString(pluginField), QString::fromUtf8(QJsonDocument(plugin).toJson(QJsonDocument::Compact)));
    if (!written)
        return std::unexpected(written.error());
    return true;
}

bool PrivateBooks::shared(const Library &library, qint64 book) { return sharedIn(said(library), library, book); }

std::expected<void, Failure> PrivateBooks::share(const Library &library, qint64 book, bool shared) {
    const std::optional<Book> found = library.book(book);
    if (!found)
        return std::unexpected(Failure(Failure::Code::NoSuchBook, {QString::number(book)}));
    if (!kindOf(kinds(library), *found))
        return std::unexpected(Failure(Failure::Code::NotKeptPrivate, {found->meta.title}));
    if (PrivateBooks::shared(library, book) == shared)
        return {};
    return append(library, Spaces::bookId(library, book), QString(sharedField), shared ? u"1"_s : u"0"_s);
}

QList<PrivateBooks::Kept> PrivateBooks::among(const Library &library, const QList<Book> &books, const Collection &collection) {
    const QList<Kind> known = kinds(library);
    if (known.isEmpty())
        return {};
    const auto all = said(library);
    QList<Kept> kept;
    for (const Book &book : books)
        if (const auto kind = kindOf(known, book))
            kept << Kept{.book = book.id,
                         .title = book.meta.title,
                         .kind = *kind,
                         .byRule = !byHand(collection, book.id),
                         .shared = sharedIn(all, library, book.id)};
    return kept;
}

QSet<qint64> PrivateBooks::withheld(const Library &library, const QList<Book> &books) {
    const QList<Kind> known = kinds(library);
    if (known.isEmpty())
        return {};
    const auto all = said(library);
    QSet<qint64> out;
    for (const Book &book : books)
        if (kindOf(known, book) && !sharedIn(all, library, book.id))
            out.insert(book.id);
    return out;
}

QJsonArray PrivateBooks::toJson(const QList<Kept> &kept) {
    QJsonArray shown;
    for (const Kept &book : kept)
        shown << QJsonObject{{"book", book.book},
                             {"title", book.title},
                             {"format", book.kind.format},
                             {"plugin", book.kind.plugin},
                             {"plugin_title", book.kind.title},
                             {"by_rule", book.byRule},
                             {"shared", book.shared}};
    return shown;
}
