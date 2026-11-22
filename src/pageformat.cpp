#include "pageformat.h"

#include <QFontDatabase>
#include <QLocale>
#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

#include "hyphenation.h"
#include "library.h"
#include "reader.h"
#include "readingformat.h"
#include "settings.h"

PageFormat::PageFormat(Reader *reader, Settings *settings, QObject *parent)
    : QObject(parent), m_reader(reader), m_settings(settings) {
    connect(settings, &Settings::changed, this, &PageFormat::refresh);
    // The setting of pages isn't part of the format.
    connect(settings, &Settings::changed, this, &PageFormat::relayout);
    connect(reader, &Reader::bookChanged, this, &PageFormat::refresh);
    connect(reader, &Reader::htmlChanged, this, &PageFormat::restyle);
    refresh();
}

bool PageFormat::hasBook() const { return m_reader->bookId() > 0 && m_reader->library(); }

QVariantMap PageFormat::bounds() const { return ReadingFormat::bounds().toVariantMap(); }

QStringList PageFormat::fonts() const {
    static const QStringList families = [] {
        QStringList families = QFontDatabase::families();
        families.removeIf([](const QString &family) { return QFontDatabase::isPrivateFamily(family); });
        return families;
    }();
    return families;
}

void PageFormat::setLinkColor(const QColor &color) {
    if (color == m_links)
        return;
    m_links = color;
    m_html.clear();
    restyle();
}

QString PageFormat::hyphenationLanguage() const {
    if (!m_dictionary)
        return {};
    const QString name = QLocale(QString(m_language).replace(u'-', u'_')).nativeLanguageName();
    return name.isEmpty() ? m_language : name;
}

void PageFormat::setPreferredCollection(qint64 id) {
    if (id == m_preferred)
        return;
    m_preferred = id;
    refresh();
}

void PageFormat::refresh() {
    const QJsonObject defaults = m_settings->format();
    const QJsonObject own = hasBook() ? m_reader->library()->bookFormat(m_reader->bookId()) : QJsonObject();
    // The book's collection: the one whose format is in force, else the one
    // it would be given to.
    std::optional<Collection> collection;
    if (hasBook()) {
        const Library &library = *m_reader->library();
        collection = library.formatCollection(m_reader->bookId(), m_preferred);
        if (!collection)
            if (const auto book = library.book(m_reader->bookId())) {
                const QList<Collection> in = library.collectionsOf(*book);
                const auto preferred = std::ranges::find(in, m_preferred, &Collection::id);
                if (preferred != in.end())
                    collection = *preferred;
                else if (!in.isEmpty())
                    collection = in.first();
            }
    }
    const QJsonObject collectionOwn = collection ? collection->format : QJsonObject();
    const qint64 collectionId = collection ? collection->id : 0;
    const QString collectionName = collection ? collection->name : QString();
    if (defaults == m_defaults && own == m_own && collectionOwn == m_collectionOwn && collectionId == m_collectionId
        && collectionName == m_collectionName && !m_effective.isEmpty())
        return;
    m_defaults = defaults;
    m_own = own;
    m_collectionOwn = collectionOwn;
    m_collectionId = collectionId;
    m_collectionName = collectionName;
    // The book's own over its collection's, over the defaults.
    m_effective = ReadingFormat::over(ReadingFormat::over(defaults, collectionOwn), own);
    relayout();
    emit changed();
    m_html.clear();
    restyle();
}

void PageFormat::setAvailable(double width) {
    if (qFuzzyCompare(width + 1, m_available + 1))
        return;
    m_available = width;
    relayout();
}

int PageFormat::noteFontSize() const {
    const int size = m_effective.value("font_size"_L1).toInt(ReadingFormat::range("font_size"_L1)->fallback);
    return std::max(11, static_cast<int>(std::lround(size * PageLayout::noteScale)));
}

void PageFormat::setNotes(bool notes) {
    if (notes == m_notes)
        return;
    m_notes = notes;
    relayout();
}

void PageFormat::relayout() {
    const auto number = [this](QLatin1StringView key) { return m_effective.value(key).toInt(ReadingFormat::range(key)->fallback); };
    const int size = number("font_size"_L1);
    PageLayout::Space space;
    space.available = m_available;
    space.characterWidth = PageLayout::characterWidth(PageLayout::bodyFont(m_effective.value("font_family").toString(), size));
    space.measure = number("measure"_L1);
    space.margin = number("margins"_L1) + 24;
    space.gutter = PageLayout::gutter(size);
    // A book that scrolls is one column at any width.
    const bool scrolls = m_effective.value("layout").toString() == "scroll"_L1;
    space.notes = m_notes;
    space.folded = m_settings->marginFolded();
    const PageLayout::Columns columns = PageLayout::columns(space, scrolls ? 1 : m_settings->pages(), m_columns.count);
    if (space == m_space && columns == m_columns)
        return;
    m_space = space;
    m_columns = columns;
    emit layoutChanged();
}

bool PageFormat::followLanguage() {
    std::optional<Book> book;
    if (hasBook())
        book = m_reader->library()->book(m_reader->bookId());
    const QString language = book ? book->meta.language : QString();
    if (language == m_language)
        return false;
    m_language = language;
    m_dictionary = Hyphenation::forLanguage(language);
    emit changed();
    return true;
}

void PageFormat::restyle() {
    const QString chapter = m_reader->html();
    // Another book, maybe in another language, or its language set anew.
    const bool relanguaged = followLanguage();
    if (!m_html.isEmpty() && chapter == m_chapter && !relanguaged)
        return;
    m_chapter = chapter;
    m_html = ReadingFormat::setChapter(chapter, m_effective, m_links, m_dictionary);
    emit htmlChanged();
}

QString PageFormat::set(const QString &key, const QVariant &value, bool forBook) {
    return setFor(key, value, forBook ? u"book"_s : u"defaults"_s);
}

QString PageFormat::setFor(const QString &key, const QVariant &value, const QString &where) {
    const QJsonValue json = QJsonValue::fromVariant(value);
    // A number from QML may come as a double that is a whole one.
    const QJsonValue given = json.isDouble() ? QJsonValue(qRound64(json.toDouble())) : json;
    if (const QString why = ReadingFormat::problem(key, given); !why.isEmpty())
        return why;
    if (where == "collection"_L1) {
        if (!hasBook() || m_collectionId == 0)
            return "The open book is in no collection to give it to.";
        QJsonObject options = m_collectionOwn;
        options.insert(key, given);
        if (!m_reader->library()->setCollectionFormat(m_collectionId, options))
            return "The library can't be written.";
        refresh();
        return {};
    }
    if (where != "book"_L1) {
        m_settings->setOption(key, given);
        return {};
    }
    if (!hasBook())
        return "No book is open to give it its own.";
    QJsonObject own = m_own;
    own.insert(key, given);
    if (!m_reader->library()->setBookFormat(m_reader->bookId(), own))
        return "The library can't be written.";
    refresh();
    return {};
}

void PageFormat::resetBook() {
    if (!hasBook() || m_own.isEmpty())
        return;
    m_reader->library()->setBookFormat(m_reader->bookId(), {});
    refresh();
}

void PageFormat::resetCollection() {
    if (!hasBook() || m_collectionId == 0 || m_collectionOwn.isEmpty())
        return;
    m_reader->library()->setCollectionFormat(m_collectionId, {});
    refresh();
}

QVariantMap PageFormat::snapshot() const {
    return {{"defaults", m_defaults.toVariantMap()}, {"own", m_own.toVariantMap()}, {"book", m_reader->bookId()},
            {"collection", m_collectionId}, {"collectionOwn", m_collectionOwn.toVariantMap()}};
}

void PageFormat::restore(const QVariantMap &snapshot) {
    const QJsonObject defaults = QJsonObject::fromVariantMap(snapshot.value("defaults").toMap());
    for (auto it = defaults.begin(); it != defaults.end(); ++it)
        m_settings->setOption(it.key(), it.value());
    if (hasBook() && snapshot.value("book").toLongLong() == m_reader->bookId())
        m_reader->library()->setBookFormat(m_reader->bookId(), QJsonObject::fromVariantMap(snapshot.value("own").toMap()));
    if (hasBook() && snapshot.value("collection").toLongLong() > 0)
        m_reader->library()->setCollectionFormat(snapshot.value("collection").toLongLong(),
                                                 QJsonObject::fromVariantMap(snapshot.value("collectionOwn").toMap()));
    refresh();
}

QString PageFormat::from(const QString &key) const {
    return m_own.contains(key) ? u"book"_s : m_collectionOwn.contains(key) ? u"collection"_s : u"defaults"_s;
}

void PageFormat::zoom(int steps) {
    const ReadingFormat::Range *size = ReadingFormat::range(u"font_size");
    setFor("font_size", std::clamp(m_effective.value("font_size").toInt(size->fallback) + steps, size->low, size->high), from("font_size"));
}

QString PageFormat::nextLayout() {
    const auto &layouts = ReadingFormat::layouts;
    const auto now = std::ranges::find(layouts, m_effective.value("layout").toString());
    const QString next = now == layouts.end() || now + 1 == layouts.end() ? QString(layouts.front()) : QString(*(now + 1));
    setFor("layout", next, from("layout"));
    return next;
}
