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
#include <cmath>

#include "formats.h"

namespace {

const QStringList &roles() {
    static const QStringList roles{"bookId", "title", "authors", "series", "seriesIndex", "publisher", "language",
                                   "isbn", "published", "description", "tags", "formats", "formatNames", "cover",
                                   "percent", "folder", "paper", "held", "status", "shelf", "rating", "finished",
                                   "added", "lastRead",
                                   // These two are worked out when a view asks, not for every book up front.
                                   "spineColor", "thickness"};
    return roles;
}

const QString none = "-";

// The orders that start with the most: the newest, the furthest read, the best.
bool startsHigh(const QString &sort) {
    return QStringList{"recent", "added", "progress", "rating", "published", "finished"}.contains(sort);
}

// A number in [0, 1) that belongs to a book and a purpose, the same on every
// run: what makes one spine a little taller than the next, for good.
double unit(qint64 id, quint32 purpose) {
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
    return {{"cover", book.value("cover")}, {"title", book.value("title")}, {"authors", book.value("authors")}};
}

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
    };
}

BookListModel::BookListModel(QObject *parent) : QAbstractListModel(parent) {}

BookListModel::~BookListModel() {
    if (!m_colorsChanged)
        return;
    QJsonObject colors;
    for (auto it = m_colors.cbegin(); it != m_colors.cend(); ++it)
        colors.insert(it.key(), it.value().name());
    QDir().mkpath(QFileInfo(colorCache()).absolutePath());
    QSaveFile file(colorCache());
    if (file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(colors).toJson(QJsonDocument::Compact)) > 0)
        file.commit();
}

QStringList BookListModel::sorts() {
    return {"recent", "title", "author", "series", "added", "progress", "rating", "published", "finished"};
}

QStringList BookListModel::filterKinds() {
    return {"status", "held", "rating", "format", "language", "tag", "author", "series"};
}

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
    for (int i = 0; i < roles().size(); ++i)
        names.insert(Qt::UserRole + i, roles().at(i).toLatin1());
    return names;
}

void BookListModel::setBooks(const QList<Book> &books, const QString &libraryRoot) {
    QList<QVariantMap> maps;
    maps.reserve(books.size());
    for (const Book &book : books)
        maps << bookToMap(book, libraryRoot);
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
    if (sort == m_sort || !sorts().contains(sort))
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

bool BookListModel::descending() const { return startsHigh(m_sort) != m_reversed; }

void BookListModel::setFilters(const QVariantMap &filters) {
    // Only what can be filtered on, and only values that say something.
    QVariantMap kept;
    for (const QString &kind : filterKinds()) {
        const QVariant value = filters.value(kind);
        if (kind == "rating" ? value.toInt() > 0 : !value.toString().isEmpty())
            kept.insert(kind, kind == "rating" ? QVariant(qBound(1, value.toInt(), 5)) : QVariant(value.toString()));
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
    for (int row = 0; row < m_rows.size(); ++row)
        if (m_books.at(m_rows.at(row)).id == id)
            return row;
    return -1;
}

QVariantMap BookListModel::get(int row) const {
    if (row < 0 || row >= m_rows.size())
        return {};
    QVariantMap book = m_maps.at(m_rows.at(row));
    book.insert("spineColor", spineColor(m_rows.at(row)));
    return book;
}

bool BookListModel::passes(const Book &book, const QString &kind, const QVariant &value) const {
    const QString wanted = value.toString();
    auto among = [&](const QStringList &values) {
        return wanted == none ? values.isEmpty() : values.contains(wanted, Qt::CaseInsensitive);
    };
    if (kind == "status")
        return wanted == "unread" ? book.status() != "read" && book.status() != "reading"
               : wanted == none ? book.status().isEmpty() : book.status() == wanted;
    if (kind == "held")
        return wanted == "digital" ? !book.formats.isEmpty() : wanted == "paper" ? book.paper : book.held() == "none";
    if (kind == "rating")
        return book.rating >= value.toInt();
    if (kind == "format")
        return book.formats.contains(wanted, Qt::CaseInsensitive);
    if (kind == "language")
        return among(book.meta.language.isEmpty() ? QStringList() : QStringList{book.meta.language});
    if (kind == "tag")
        return among(book.meta.tags);
    if (kind == "author")
        return among(book.meta.authors);
    if (kind == "series")
        return among(book.meta.series.isEmpty() ? QStringList() : QStringList{book.meta.series});
    return true;
}

QList<int> BookListModel::arranged(const QString &except) const {
    QList<int> shown;
    const bool searching = !m_query.trimmed().isEmpty();
    for (int i = 0; i < m_books.size(); ++i) {
        const Book &book = m_books.at(i);
        bool fits = !searching || book.matches(m_query);
        for (auto filter = m_filters.cbegin(); fits && filter != m_filters.cend(); ++filter)
            fits = filter.key() == except || passes(book, filter.key(), filter.value());
        if (fits)
            shown << i;
    }

    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    collator.setNumericMode(true);
    auto number = [](double a, double b) { return a < b ? -1 : a > b ? 1 : 0; };
    // What a book is sorted by may be missing (no series, never read, not
    // rated); those come last whichever way the order runs.
    auto missing = [&](const Book &book) {
        return m_sort == "recent" ? book.lastRead == 0
               : m_sort == "author" ? book.meta.authors.isEmpty()
               : m_sort == "series" ? book.meta.series.isEmpty()
               : m_sort == "progress" ? book.percent <= 0
               : m_sort == "rating" ? book.rating == 0
               : m_sort == "published" ? book.meta.published.isEmpty()
               : m_sort == "finished" ? book.finished.isEmpty()
               : false;
    };
    auto compare = [&](const Book &a, const Book &b) {
        if (m_sort == "title")
            return collator.compare(a.meta.title, b.meta.title);
        if (m_sort == "author") {
            // An author's books together, and a series in its order.
            if (const int authors = collator.compare(lastNameFirst(a.meta.authors), lastNameFirst(b.meta.authors)))
                return authors;
            if (const int series = collator.compare(a.meta.series, b.meta.series))
                return series;
            return number(a.meta.seriesIndex, b.meta.seriesIndex);
        }
        if (m_sort == "series") {
            if (const int series = collator.compare(a.meta.series, b.meta.series))
                return series;
            return number(a.meta.seriesIndex, b.meta.seriesIndex);
        }
        if (m_sort == "added")
            return a.added != b.added ? number(a.added, b.added) : number(a.id, b.id);
        if (m_sort == "progress")
            return number(a.percent, b.percent);
        if (m_sort == "rating")
            return number(a.rating, b.rating);
        if (m_sort == "published")
            return a.meta.published.compare(b.meta.published);
        if (m_sort == "finished")
            return a.finished.compare(b.finished);
        return number(a.lastRead, b.lastRead);
    };
    const bool down = descending();
    // Among books that are equal, the newest first for what was read
    // lately, and the title otherwise.
    const bool newestFirst = m_sort == "recent";
    std::stable_sort(shown.begin(), shown.end(), [&](int left, int right) {
        const Book &a = m_books.at(left), &b = m_books.at(right);
        if (missing(a) != missing(b))
            return missing(b);
        if (const int order = missing(a) ? 0 : compare(a, b))
            return down ? order > 0 : order < 0;
        if (newestFirst)
            return a.id > b.id;
        if (const int titles = collator.compare(a.meta.title, b.meta.title))
            return titles < 0;
        return a.id < b.id;
    });
    return shown;
}

void BookListModel::rebuild() {
    beginResetModel();
    m_rows = arranged();
    endResetModel();
    emit countChanged();
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
    if (kind == "status" || kind == "held" || kind == "rating") {
        // A short, fixed list, in the order it is thought of.
        const QStringList values = kind == "status" ? QStringList{"reading", "to-read", "read", "unread"}
                                   : kind == "held" ? QStringList{"digital", "paper", "wishlist"}
                                   : QStringList{"5", "4", "3", "2", "1"};
        for (const QString &value : values) {
            int fitting = 0;
            for (const Book &book : std::as_const(m_books))
                fitting += passes(book, kind, value);
            if (fitting > 0)
                list << QVariantMap{{"value", kind == "rating" ? QVariant(value.toInt()) : QVariant(value)}, {"count", fitting}};
        }
        return list;
    }
    for (const Book &book : std::as_const(m_books)) {
        if (kind == "format")
            for (const QString &format : book.formats) count(format);
        else if (kind == "language")
            count(book.meta.language);
        else if (kind == "tag")
            for (const QString &tag : book.meta.tags) count(tag);
        else if (kind == "author")
            for (const QString &author : book.meta.authors) count(author);
        else if (kind == "series")
            count(book.meta.series);
    }
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::stable_sort(order.begin(), order.end(), [&](const QString &a, const QString &b) {
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
    for (const int index : arranged(kind)) {
        const Book &book = m_books.at(index);
        QStringList values = kind == "series" ? QStringList{book.meta.series}
                             : kind == "author" ? book.meta.authors
                             : kind == "tag" ? book.meta.tags
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
    std::stable_sort(order.begin(), order.end(), [&](const QString &a, const QString &b) {
        if (kind == "status")
            return statuses.indexOf(a) < statuses.indexOf(b);
        if ((a == none) != (b == none))
            return b == none;
        return collator.compare(kind == "author" ? lastNameFirst({a}) : a, kind == "author" ? lastNameFirst({b}) : b) < 0;
    });
    QVariantList list;
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
                bytes = qMax(bytes, file.size());
    const double thickness = bytes > 0 ? qBound(0.0, (std::log10(double(bytes)) - 4.7) / 2.0, 1.0) : unit(book.id, 3);
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
        lying = qMin(lying, int(m_rows.size()) - row);
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
            longest = qMax(longest, tall(row + i));
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
    for (auto it = colors.begin(); it != colors.end(); ++it)
        m_colors.insert(it.key(), QColor(it.value().toString()));
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
    QList<Bin> bins(512);
    for (int y = 0; y < small.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(small.constScanLine(y));
        for (int x = 0; x < small.width(); ++x) {
            const QColor pixel(line[x]);
            const double weight = 1 + 2 * pixel.hsvSaturationF() * pixel.valueF();
            Bin &bin = bins[(qRed(line[x]) >> 5 << 6) | (qGreen(line[x]) >> 5 << 3) | (qBlue(line[x]) >> 5)];
            bin.weight += weight;
            bin.red += weight * qRed(line[x]);
            bin.green += weight * qGreen(line[x]);
            bin.blue += weight * qBlue(line[x]);
        }
    }
    const Bin &most = *std::max_element(bins.cbegin(), bins.cend(), [](const Bin &a, const Bin &b) { return a.weight < b.weight; });
    return QColor(int(most.red / most.weight), int(most.green / most.weight), int(most.blue / most.weight));
}

QColor BookListModel::inkOn(const QColor &color) const {
    auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
    const double luminance = .2126 * linear(color.redF()) + .7152 * linear(color.greenF()) + .0722 * linear(color.blueF());
    const double hue = qMax(0.0f, color.hslHueF());
    const double saturation = qMin(0.25f, color.hslSaturationF());
    return QColor::fromHslF(hue, saturation, luminance > .179 ? 0.10 : 0.94);
}
