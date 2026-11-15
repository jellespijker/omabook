#include "booklistmodel.h"

#include <QCollator>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <algorithm>
#include <array>
#include <cmath>
#include <compare>
#include <optional>
#include <ranges>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

#include "customization.h"
#include "formats.h"

namespace {

using namespace Qt::StringLiterals;

// The orders the shelf can be in and what it can be narrowed by. Outside
// this file each goes by its name, in the settings and in the views; the
// names stand in the order of the kinds they name.
enum class Sort { Recent, Title, Author, Series, Added, Progress, Rating, Published, Finished };
constexpr std::array sortNames{"recent"_L1, "title"_L1,  "author"_L1,    "series"_L1,  "added"_L1,
                               "progress"_L1, "rating"_L1, "published"_L1, "finished"_L1};

enum class Filter { Status, Held, Rating, Format, Language, Tag, Author, Series };
constexpr std::array filterNames{"status"_L1,   "held"_L1, "rating"_L1, "format"_L1,
                                 "language"_L1, "tag"_L1,  "author"_L1, "series"_L1};

// The short, fixed lists a filter can be set to, in the order they are thought of.
constexpr std::array statusChoices{"reading"_L1, "to-read"_L1, "read"_L1, "unread"_L1};
constexpr std::array heldChoices{"digital"_L1, "paper"_L1, "wishlist"_L1};
constexpr std::array ratingChoices{"5"_L1, "4"_L1, "3"_L1, "2"_L1, "1"_L1};

template <typename Kind>
concept NamedKind = std::is_scoped_enum_v<Kind>;

// The kind a name stands for, or nothing when it names none.
template <NamedKind Kind, std::size_t Count>
[[nodiscard]] std::optional<Kind> named(const std::array<QLatin1StringView, Count> &names, QStringView name) {
    const auto found = std::ranges::find_if(names, [name](QLatin1StringView candidate) { return candidate == name; });
    return found == names.end() ? std::nullopt : std::optional(Kind(found - names.begin()));
}

[[nodiscard]] QStringList listed(std::span<const QLatin1StringView> names) { return names | std::ranges::to<QStringList>(); }

const QStringList &roles() {
    static const QStringList roles{"bookId", "title", "authors", "series", "seriesIndex", "publisher", "language",
                                   "isbn", "published", "description", "tags", "formats", "formatNames", "cover",
                                   "percent", "folder", "paper", "held", "status", "shelf", "rating", "finished",
                                   "added", "lastRead", "launcher", "plugin",
                                   // These two are worked out when a view asks, not for every book up front.
                                   "spineColor", "thickness"};
    return roles;
}

const QString none = "-";

// The orders that start with the most: the newest, the furthest read, the best.
[[nodiscard]] constexpr bool startsHigh(Sort sort) noexcept { return sort != Sort::Title && sort != Sort::Author && sort != Sort::Series; }

// What a book is sorted by may be missing (no series, never read, not
// rated); those come last whichever way the order runs.
[[nodiscard]] bool missing(const Book &book, Sort sort) {
    switch (sort) {
        using enum Sort;
    case Recent: return book.lastRead == 0;
    case Author: return book.meta.authors.isEmpty();
    case Series: return book.meta.series.isEmpty();
    case Progress: return book.percent <= 0;
    case Rating: return book.rating == 0;
    case Published: return book.meta.published.isEmpty();
    case Finished: return book.finished.isEmpty();
    case Title:
    case Added: return false;
    }
    std::unreachable();
}

// Two numbers in order; neither comes first when they can't be told apart.
[[nodiscard]] constexpr std::weak_ordering number(double a, double b) noexcept {
    return a < b ? std::weak_ordering::less : a > b ? std::weak_ordering::greater : std::weak_ordering::equivalent;
}

// Whether a book passes one filter.
[[nodiscard]] bool passes(const Book &book, Filter kind, const QVariant &value) {
    const QString wanted = value.toString();
    const auto among = [&wanted](const QStringList &values) {
        return wanted == none ? values.isEmpty() : values.contains(wanted, Qt::CaseInsensitive);
    };
    const auto only = [](const QString &value) { return value.isEmpty() ? QStringList() : QStringList{value}; };
    switch (kind) {
        using enum Filter;
    case Status:
        return wanted == "unread"_L1 ? book.status() != "read"_L1 && book.status() != "reading"_L1
               : wanted == none ? book.status().isEmpty() : book.status() == wanted;
    case Held: return wanted == "digital"_L1 ? !book.formats.isEmpty() : wanted == "paper"_L1 ? book.paper : book.held() == "none"_L1;
    case Rating: return book.rating >= value.toInt();
    case Format: return book.formats.contains(wanted, Qt::CaseInsensitive);
    case Language: return among(only(book.meta.language));
    case Tag: return among(book.meta.tags);
    case Author: return among(book.meta.authors);
    case Series: return among(only(book.meta.series));
    }
    std::unreachable();
}

// A number in [0, 1) that belongs to a book and a purpose, the same on every
// run: what makes one spine a little taller than the next, for good.
[[nodiscard]] constexpr double unit(qint64 id, quint32 purpose) noexcept {
    quint64 x = quint64(id) * 0x9E3779B97F4A7C15ull + purpose * 0xBF58476D1CE4E5B9ull;
    x ^= x >> 31;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 29;
    return double(x % 10000) / 10000.0;
}

// "Le Guin, Ursula K." and "Ursula K. Le Guin" both file under their last
// word, which is where a shelf by author puts them.
QString lastNameFirst(const QStringList &authors) {
    const QString first = authors.value(0).simplified();
    if (first.contains(','))
        return first;
    const qsizetype space = first.lastIndexOf(' ');
    return space < 0 ? first : first.mid(space + 1) + ", " + first.left(space);
}

QString colorCache() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/omabook/spine-colors.json";
}

QVariantMap brief(const QVariantMap &book) {
    return {{"cover", book.value("cover")}, {"title", book.value("title")}, {"authors", book.value("authors")},
            {"launcher", book.value("launcher")}, {"held", book.value("held")}};
}

// A launcher is no book, so it is only ever found by the words on its
// cover: nothing that asks after a book's format, status or rating finds it.
bool launcherMatches(const Book &launcher, const QString &query) {
    const QString words = launcher.meta.title + "\n" + launcher.meta.authorText();
    return std::ranges::all_of(query.split(' ', Qt::SkipEmptyParts),
                               [&words](const QString &word) { return !word.contains(':') && words.contains(word, Qt::CaseInsensitive); });
}

[[nodiscard]] bool isLauncher(const Book &book) noexcept { return book.id < 0; }

} // namespace

QVariantMap bookToMap(const Book &book, const QString &libraryRoot) {
    const QString folder = libraryRoot + "/" + book.path;
    QUrl cover;
    if (book.hasCover) {
        // The cover's time in the address makes a replaced cover a new picture
        // to the image cache.
        const QFileInfo file(folder + "/cover.jpg");
        if (file.exists()) {
            cover = QUrl::fromLocalFile(file.filePath());
            cover.setQuery(QString::number(file.lastModified().toSecsSinceEpoch()));
        }
    }
    QStringList names;
    for (const QString &format : book.formats)
        if (!names.contains(Formats::name(format)))
            names << Formats::name(format);
    return {
        {"bookId", book.id},
        {"title", book.meta.title},
        {"authors", book.meta.authorText()},
        {"series", book.meta.series},
        {"seriesIndex", book.meta.seriesIndex},
        {"publisher", book.meta.publisher},
        {"language", book.meta.language},
        {"isbn", book.meta.isbn},
        {"published", book.meta.published},
        {"description", book.meta.description},
        {"tags", book.meta.tags.join(", ")},
        {"formats", book.formats},
        {"formatNames", names.join(", ")},
        {"cover", cover},
        {"percent", book.percent},
        {"folder", folder},
        // Whether there is a copy on a real shelf, and what is held of it
        // at all: "digital", "paper", "both" or "none" (a wish).
        {"paper", book.paper},
        {"held", book.held()},
        // "to-read", "reading", "read" or empty; `shelf` is only what was
        // said of it, without how far it was read here.
        {"status", book.status()},
        {"shelf", book.shelf},
        {"rating", book.rating},
        {"finished", book.finished},
        // Seconds since the epoch, 0 when never.
        {"added", book.added},
        {"lastRead", book.lastRead},
        {"launcher", false},
        {"plugin", QString()},
    };
}

Book launcherBook(const QVariantMap &launcher) {
    Book book;
    book.id = launcher.value("bookId").toLongLong();
    book.meta.title = launcher.value("title").toString();
    book.meta.authors = QStringList{launcher.value("authors").toString()};
    book.meta.authors.removeAll(QString());
    book.meta.description = launcher.value("description").toString();
    return book;
}

QVariantMap launcherToMap(const QVariantMap &launcher) {
    QVariantMap map = bookToMap(launcherBook(launcher), {});
    map.insert("cover", launcher.value("cover").toUrl());
    map.insert("folder", QString());
    // Not a file, not paper, not a wish: something to start.
    map.insert("held", QStringLiteral("launcher"));
    map.insert("launcher", true);
    map.insert("plugin", launcher.value("plugin"));
    map.insert("pluginName", launcher.value("pluginName"));
    return map;
}

BookListModel::BookListModel(QObject *parent) : QAbstractListModel(parent) {}

BookListModel::~BookListModel() {
    if (!m_colorsChanged)
        return;
    QJsonObject colors;
    for (const auto &[cover, color] : m_colors.asKeyValueRange())
        colors.insert(cover, color.name());
    QDir().mkpath(QFileInfo(colorCache()).absolutePath());
    QSaveFile file(colorCache());
    if (file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(colors).toJson(QJsonDocument::Compact)) > 0)
        file.commit();
}

QStringList BookListModel::sorts() { return listed(sortNames); }

QStringList BookListModel::filterKinds() { return listed(filterNames); }

int BookListModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : m_rows.size(); }

QVariant BookListModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_rows.size() || role < Qt::UserRole)
        return {};
    const QString name = roles().value(role - Qt::UserRole);
    const int book = m_rows.at(index.row());
    if (name == "spineColor")
        return spineColor(book);
    if (name == "thickness")
        return thickness(book);
    return m_maps.at(book).value(name);
}

QHash<int, QByteArray> BookListModel::roleNames() const {
    QHash<int, QByteArray> names;
    for (const auto &[index, role] : std::views::enumerate(roles()))
        names.insert(Qt::UserRole + int(index), role.toLatin1());
    return names;
}

void BookListModel::setBooks(const QList<Book> &books, const QString &libraryRoot) {
    QList<QVariantMap> maps;
    maps.reserve(books.size());
    for (const Book &book : books)
        maps << bookToMap(book, libraryRoot);
    m_real = books;
    m_realMaps = maps;
    present(libraryRoot);
}

void BookListModel::setLaunchers(const QVariantList &launchers) {
    QList<QVariantMap> maps;
    for (const QVariant &launcher : launchers)
        maps << launcher.toMap();
    if (maps == m_launchers)
        return;
    m_launchers = maps;
    present(m_root);
}

void BookListModel::setMembers(const std::optional<QSet<qint64>> &members) {
    if (members == m_members)
        return;
    m_members = members;
    rebuild();
}

bool BookListModel::member(const Book &book) const { return !m_members || m_members->contains(book.id); }

void BookListModel::present(const QString &libraryRoot) {
    // The launchers ride along with the books, after them; the order the
    // shelf shows puts them first.
    QList<Book> books = m_real;
    QList<QVariantMap> maps = m_realMaps;
    for (const QVariantMap &launcher : std::as_const(m_launchers)) {
        books << launcherBook(launcher);
        maps << launcherToMap(launcher);
    }
    // The library is read again whenever its database is written, which is
    // also when the knowledge graph grows or a reading place is saved. When
    // nothing the shelf shows has changed, the shelf is left alone, so it
    // keeps its place under the reader's hand.
    if (libraryRoot == m_root && maps == m_maps)
        return;
    QList<qint64> shownBefore;
    shownBefore.reserve(m_rows.size());
    for (const int row : std::as_const(m_rows))
        shownBefore << m_books.at(row).id;
    const QList<QVariantMap> mapsBefore = m_maps;
    const QList<int> rowsBefore = m_rows;
    const bool sameLibrary = libraryRoot == m_root;

    m_books = books;
    m_root = libraryRoot;
    m_maps = maps;
    // A book may have been given another file since.
    m_thickness.clear();
    const QList<int> rows = arranged();
    QList<qint64> shownNow;
    shownNow.reserve(rows.size());
    for (const int row : rows)
        shownNow << m_books.at(row).id;
    if (!sameLibrary || shownNow != shownBefore) {
        rebuild();
        return;
    }
    // The same books in the same order: only the ones that changed are
    // redrawn, and the view keeps its scroll and its selection.
    m_rows = rows;
    for (int row = 0; row < m_rows.size(); ++row)
        if (m_maps.at(m_rows.at(row)) != mapsBefore.at(rowsBefore.at(row)))
            emit dataChanged(index(row), index(row));
}

void BookListModel::setQuery(const QString &query) {
    if (query == m_query)
        return;
    m_query = query;
    emit queryChanged();
    rebuild();
}

void BookListModel::setSort(const QString &sort) {
    if (sort == m_sort || !named<Sort>(sortNames, sort))
        return;
    m_sort = sort;
    emit sortChanged();
    rebuild();
}

void BookListModel::setReversed(bool reversed) {
    if (reversed == m_reversed)
        return;
    m_reversed = reversed;
    emit sortChanged();
    rebuild();
}

// The order is only ever set to one of sorts(), so its name always names one.
bool BookListModel::descending() const { return startsHigh(named<Sort>(sortNames, m_sort).value_or(Sort::Recent)) != m_reversed; }

void BookListModel::setFilters(const QVariantMap &filters) {
    // Only what can be filtered on, and only values that say something.
    QVariantMap kept;
    for (const auto &[index, kind] : std::views::enumerate(filterNames)) {
        const QVariant value = filters.value(kind);
        if (Filter(index) == Filter::Rating) {
            if (value.toInt() > 0)
                kept.insert(kind, std::clamp(value.toInt(), 1, 5));
        } else if (!value.toString().isEmpty()) {
            kept.insert(kind, value.toString());
        }
    }
    if (kept == m_filters)
        return;
    m_filters = kept;
    emit filtersChanged();
    rebuild();
}

void BookListModel::setFilter(const QString &kind, const QVariant &value) {
    QVariantMap filters = m_filters;
    filters.insert(kind, value);
    setFilters(filters);
}

void BookListModel::clearFilters() { setFilters({}); }

qint64 BookListModel::idAt(int row) const { return row >= 0 && row < m_rows.size() ? m_books.at(m_rows.at(row)).id : 0; }

int BookListModel::rowOf(qint64 id) const {
    const auto found = std::ranges::find(m_rows, id, [this](int book) { return m_books.at(book).id; });
    return found == m_rows.cend() ? -1 : int(found - m_rows.cbegin());
}

QVariantMap BookListModel::get(int row) const {
    if (row < 0 || row >= m_rows.size())
        return {};
    QVariantMap book = m_maps.at(m_rows.at(row));
    book.insert("spineColor", spineColor(m_rows.at(row)));
    return book;
}

QList<int> BookListModel::arranged(const QString &except) const {
    // Every filter but one of `except`'s kind, told apart once rather than for every book.
    struct Narrowing {
        Filter kind;
        QVariant value;
    };
    QList<Narrowing> narrowings;
    for (const auto &[name, value] : m_filters.asKeyValueRange())
        if (const auto kind = named<Filter>(filterNames, name); kind && name != except)
            narrowings.append({*kind, value});
    const bool searching = !m_query.trimmed().isEmpty();
    auto shown = std::views::iota(0, int(m_books.size())) | std::views::filter([this, searching, &narrowings](int index) {
                     const Book &book = m_books.at(index);
                     // A launcher is on the whole shelf, and in a search
                     // for its words, but never among books narrowed down.
                     if (isLauncher(book))
                         return !m_members && narrowings.isEmpty() && (!searching || launcherMatches(book, m_query));
                     return member(book) && (!searching || book.matches(m_query))
                            && std::ranges::all_of(narrowings, [&book](const Narrowing &narrowing) {
                                   return passes(book, narrowing.kind, narrowing.value);
                               });
                 })
                 | std::ranges::to<QList<int>>();

    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    collator.setNumericMode(true);
    const auto text = [&collator](const QString &a, const QString &b) -> std::weak_ordering { return collator.compare(a, b) <=> 0; };
    // A series together, in its order.
    const auto inSeries = [&text](const Book &a, const Book &b) {
        const auto series = text(a.meta.series, b.meta.series);
        return series != 0 ? series : number(a.meta.seriesIndex, b.meta.seriesIndex);
    };
    const Sort sort = named<Sort>(sortNames, m_sort).value_or(Sort::Recent);
    const auto compare = [&text, &inSeries, sort](const Book &a, const Book &b) -> std::weak_ordering {
        switch (sort) {
            using enum Sort;
        case Title: return text(a.meta.title, b.meta.title);
        case Author: {
            // An author's books together, and a series in its order.
            const auto authors = text(lastNameFirst(a.meta.authors), lastNameFirst(b.meta.authors));
            return authors != 0 ? authors : inSeries(a, b);
        }
        case Series: return inSeries(a, b);
        case Added: return std::tie(a.added, a.id) <=> std::tie(b.added, b.id);
        case Progress: return number(a.percent, b.percent);
        case Rating: return a.rating <=> b.rating;
        case Published: return a.meta.published.compare(b.meta.published) <=> 0;
        case Finished: return a.finished.compare(b.finished) <=> 0;
        case Recent: return a.lastRead <=> b.lastRead;
        }
        std::unreachable();
    };
    const bool down = startsHigh(sort) != m_reversed;
    // Among books that are equal, the newest first for what was read
    // lately, and the title otherwise.
    const bool newestFirst = sort == Sort::Recent;
    std::ranges::stable_sort(
        shown,
        [&](const Book &a, const Book &b) {
            const bool without = missing(a, sort);
            if (without != missing(b, sort))
                return !without;
            if (const auto order = without ? std::weak_ordering::equivalent : compare(a, b); order != 0)
                return down ? order > 0 : order < 0;
            if (newestFirst)
                return a.id > b.id;
            if (const auto titles = text(a.meta.title, b.meta.title); titles != 0)
                return titles < 0;
            return a.id < b.id;
        },
        [this](int index) -> const Book & { return m_books.at(index); });
    // Whatever the order, the launchers are where the shelf starts, in the
    // order their plugins put them there.
    const auto books = std::ranges::stable_partition(shown, [this](int index) { return isLauncher(m_books.at(index)); });
    std::ranges::sort(shown.begin(), books.begin());
    return shown;
}

void BookListModel::rebuild() {
    beginResetModel();
    m_rows = arranged();
    endResetModel();
    emit countChanged();
}

int BookListModel::shownBooks() const {
    return int(std::ranges::count_if(m_rows, [this](int row) { return !isLauncher(m_books.at(row)); }));
}

QVariantList BookListModel::choices(const QString &kind) const {
    QStringList order;
    QHash<QString, int> counts;
    auto count = [&](const QString &value) {
        if (value.isEmpty())
            return;
        if (!counts.contains(value))
            order << value;
        ++counts[value];
    };
    QVariantList list;
    const auto by = named<Filter>(filterNames, kind);
    if (!by)
        return list;
    if (by == Filter::Status || by == Filter::Held || by == Filter::Rating) {
        // A short, fixed list, in the order it is thought of.
        using Choices = std::span<const QLatin1StringView>;
        const Choices values = by == Filter::Status ? Choices(statusChoices) : by == Filter::Held ? Choices(heldChoices) : Choices(ratingChoices);
        for (const QString value : values) {
            const auto fitting = std::ranges::count_if(m_real, [&](const Book &book) { return member(book) && passes(book, *by, value); });
            if (fitting > 0)
                list << QVariantMap{{"value", by == Filter::Rating ? QVariant(value.toInt()) : QVariant(value)}, {"count", int(fitting)}};
        }
        return list;
    }
    for (const Book &book : std::as_const(m_real)) {
        if (!member(book))
            continue;
        switch (*by) {
            using enum Filter;
        case Format: std::ranges::for_each(book.formats, count); break;
        case Language: count(book.meta.language); break;
        case Tag: std::ranges::for_each(book.meta.tags, count); break;
        case Author: std::ranges::for_each(book.meta.authors, count); break;
        case Series: count(book.meta.series); break;
        case Status:
        case Held:
        case Rating: break;
        }
    }
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    // The most common first, and those as common as each other by name.
    std::ranges::stable_sort(order, [&](const QString &a, const QString &b) {
        return counts.value(a) != counts.value(b) ? counts.value(a) > counts.value(b) : collator.compare(a, b) < 0;
    });
    for (const QString &value : std::as_const(order))
        list << QVariantMap{{"value", value}, {"count", counts.value(value)}};
    return list;
}

QVariantList BookListModel::groups(const QString &kind) const {
    struct Pile {
        int count = 0;
        QVariantList books;
    };
    QStringList order;
    QHash<QString, Pile> piles;
    const auto by = named<Filter>(filterNames, kind);
    // A launcher is a pile of its own, before the others, that opens itself.
    QVariantList launchers;
    for (const int index : arranged(kind)) {
        const Book &book = m_books.at(index);
        if (isLauncher(book)) {
            launchers << QVariantMap{{"value", QString()}, {"launcher", book.id}, {"title", book.meta.title},
                                     {"authors", book.meta.authorText()}, {"count", 0}, {"books", QVariantList{brief(m_maps.at(index))}}};
            continue;
        }
        QStringList values = by == Filter::Series ? QStringList{book.meta.series}
                             : by == Filter::Author ? book.meta.authors
                             : by == Filter::Tag ? book.meta.tags
                             : QStringList{book.status()};
        values.removeAll(QString());
        if (values.isEmpty())
            values << none;
        for (const QString &value : std::as_const(values)) {
            if (!piles.contains(value))
                order << value;
            Pile &pile = piles[value];
            ++pile.count;
            if (pile.books.size() < 3)
                pile.books << brief(m_maps.at(index));
        }
    }
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    collator.setNumericMode(true);
    const QStringList statuses{"reading", "to-read", "read", none};
    if (by == Filter::Status) {
        std::ranges::stable_sort(order, {}, [&statuses](const QString &status) { return statuses.indexOf(status); });
    } else {
        // An author is filed under their last name, as on the shelf.
        const auto filed = [by](const QString &value) { return by == Filter::Author ? lastNameFirst({value}) : value; };
        std::ranges::stable_sort(order, [&](const QString &a, const QString &b) {
            if ((a == none) != (b == none))
                return b == none;
            return collator.compare(filed(a), filed(b)) < 0;
        });
    }
    QVariantList list = launchers;
    for (const QString &value : std::as_const(order))
        list << QVariantMap{{"value", value}, {"count", piles.value(value).count}, {"books", piles.value(value).books}};
    return list;
}

double BookListModel::thickness(int index) const {
    const Book &book = m_books.at(index);
    const auto known = m_thickness.constFind(book.id);
    if (known != m_thickness.constEnd())
        return *known;
    // The library keeps no page count, so a book is as thick as its largest
    // file is big: 50 kB is a pamphlet and 5 MB a doorstop. A book with no
    // file gets a thickness of its own, the same every time.
    qint64 bytes = 0;
    if (!book.formats.isEmpty())
        for (const QFileInfo &file : QDir(m_root + "/" + book.path).entryInfoList(QDir::Files))
            if (book.formats.contains(file.suffix().toLower()))
                bytes = std::max(bytes, file.size());
    const double thickness = bytes > 0 ? std::clamp((std::log10(double(bytes)) - 4.7) / 2.0, 0.0, 1.0) : unit(book.id, 3);
    m_thickness.insert(book.id, thickness);
    return thickness;
}

QVariantList BookListModel::shelve(qreal width, qreal height) const {
    QVariantList shelves;
    QVariantList shelf;
    qreal x = 0;
    auto wrap = [&](qreal needed) {
        if (x > 0 && x + needed > width) {
            shelves << QVariant(shelf);
            shelf.clear();
            x = 0;
        }
    };
    auto thick = [&](int row) { return std::round(height * (0.10 + 0.16 * thickness(m_rows.at(row)))); };
    auto tall = [&](int row) { return std::round(height * (0.80 + 0.20 * unit(m_books.at(m_rows.at(row)).id, 1))); };

    for (int row = 0; row < m_rows.size();) {
        const qint64 id = m_books.at(m_rows.at(row)).id;
        // About one book in eight starts a pile of two to four lying flat,
        // as long as the pile stays lower than a standing book.
        int lying = unit(id, 2) < 0.13 ? 2 + int(unit(id, 4) * 3) : 0;
        lying = std::min(lying, int(m_rows.size()) - row);
        qreal pile = 0;
        for (int i = 0; i < lying; ++i)
            pile += thick(row + i);
        if (lying < 2 || pile > height * 0.8) {
            const qreal w = thick(row);
            wrap(w);
            shelf << QVariantMap{{"row", row}, {"x", x}, {"y", 0}, {"w", w}, {"h", tall(row)}, {"flat", false}};
            x += w;
            ++row;
            continue;
        }
        qreal longest = 0;
        for (int i = 0; i < lying; ++i)
            longest = std::max(longest, tall(row + i));
        // A pile is never squared up: each book sits a little off the one below.
        const qreal slack = std::round(height * 0.06), gap = std::round(height * 0.03);
        wrap(longest + slack + 2 * gap);
        x += x > 0 ? gap : 0;
        // The first of them lies on top, so reading down the pile is reading on.
        qreal y = pile;
        for (int i = 0; i < lying; ++i) {
            const qreal h = thick(row + i);
            y -= h;
            const qreal off = std::round(unit(m_books.at(m_rows.at(row + i)).id, 5) * slack);
            shelf << QVariantMap{{"row", row + i}, {"x", x + off}, {"y", y}, {"w", tall(row + i)}, {"h", h}, {"flat", true}};
        }
        x += longest + slack + gap;
        row += lying;
    }
    if (!shelf.isEmpty())
        shelves << QVariant(shelf);
    return shelves;
}

void BookListModel::loadColors() const {
    if (m_colorsLoaded)
        return;
    m_colorsLoaded = true;
    QFile file(colorCache());
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject colors = QJsonDocument::fromJson(file.readAll()).object();
    // Covers come and go; a cache grown past any library starts over.
    if (colors.size() > 20000)
        return;
    for (auto color = colors.begin(); color != colors.end(); ++color)
        m_colors.insert(color.key(), QColor(color.value().toString()));
}

QColor BookListModel::spineColor(int index) const {
    const QUrl cover = m_maps.at(index).value("cover").toUrl();
    if (cover.isEmpty())
        return {};
    loadColors();
    const QString key = cover.toString();
    const auto known = m_colors.constFind(key);
    if (known != m_colors.constEnd())
        return *known;
    // Read small: a JPEG decodes at a fraction of its size in a fraction of
    // the time, and a color needs no more than a thumbnail.
    QImageReader reader(cover.toLocalFile());
    reader.setScaledSize(QSize(24, 36));
    const QColor color = dominantColor(reader.read());
    m_colors.insert(key, color);
    m_colorsChanged = true;
    return color;
}

QColor BookListModel::dominantColor(const QImage &image) {
    if (image.isNull())
        return {};
    const QImage small = image.scaled(24, 36, Qt::IgnoreAspectRatio, Qt::FastTransformation).convertToFormat(QImage::Format_RGB32);
    // Pixels are sorted into 512 coarse colors and the fullest bin wins,
    // with strong colors counting for more: a cover is remembered by its
    // red, not by the white the title is set on.
    struct Bin {
        double weight = 0, red = 0, green = 0, blue = 0;
    };
    std::array<Bin, 512> bins;
    for (int y = 0; y < small.height(); ++y) {
        // A line of an RGB32 picture is that many pixels, one after the other.
        const std::span line(reinterpret_cast<const QRgb *>(small.constScanLine(y)), std::size_t(small.width()));
        for (const QRgb pixel : line) {
            const QColor color(pixel);
            const double weight = 1 + 2 * color.hsvSaturationF() * color.valueF();
            Bin &bin = bins[(qRed(pixel) >> 5 << 6) | (qGreen(pixel) >> 5 << 3) | (qBlue(pixel) >> 5)];
            bin.weight += weight;
            bin.red += weight * qRed(pixel);
            bin.green += weight * qGreen(pixel);
            bin.blue += weight * qBlue(pixel);
        }
    }
    const Bin &most = *std::ranges::max_element(bins, {}, &Bin::weight);
    return QColor(int(most.red / most.weight), int(most.green / most.weight), int(most.blue / most.weight));
}

QColor BookListModel::inkOn(const QColor &color) const {
    const double hue = std::max(0.0f, color.hslHueF());
    const double saturation = std::min(0.25f, color.hslSaturationF());
    return QColor::fromHslF(hue, saturation, relativeLuminance(color) > .179 ? 0.10 : 0.94);
}
