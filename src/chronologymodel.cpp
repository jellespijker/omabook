#include "chronologymodel.h"

#include <QMap>
#include <algorithm>
#include <ranges>

#include "backend.h"
#include "chronologycard.h"
#include "graphmodel.h"

using namespace Chronology;
using namespace Qt::StringLiterals;
using Reading::From;

namespace {

// How many of a long list the filter's list shows before anything is typed.
constexpr int common = 6;

// A stretch of a band's calendar a range asks for, in minutes; nothing for
// a band of no calendar, which a range leaves out.
struct Stretch {
    std::optional<double> from, to;
};

[[nodiscard]] std::optional<Stretch> stretchOn(const Chart::Band &band, const QString &from, const QString &to) {
    if (!band.dated())
        return std::nullopt;
    const auto edge = [&band](const QString &text, bool start) -> std::optional<double> {
        const auto point = EventTime::Point::parse(QStringView(text), band.calendar == EventTime::gregorianName());
        if (text.isEmpty() || !point)
            return std::nullopt;
        EventTime time;
        time.on = {.calendar = band.calendar, .at = *point, .from = {}, .to = {}};
        const auto extent = band.scale->extent(time);
        if (!extent)
            return std::nullopt;
        // From the start of its first moment to the end of its last.
        const std::optional<qint64> minute = start ? extent->from : extent->to;
        return minute.transform([](qint64 at) { return static_cast<double>(at); });
    };
    return Stretch{.from = edge(from, true), .to = edge(to, false)};
}

[[nodiscard]] bool parses(const QString &text) {
    return text.isEmpty() || EventTime::Point::parse(QStringView(text), true) || EventTime::Point::parse(QStringView(text), false);
}

} // namespace

ChronologyBrowser::ChronologyBrowser(QObject *parent) : QObject(parent) {
    // While the graph is being written it changes many times a second; the
    // chronology is read again once it settles.
    m_refreshSoon.setSingleShot(true);
    m_refreshSoon.setInterval(400);
    connect(&m_refreshSoon, &QTimer::timeout, this, &ChronologyBrowser::refresh);
    connect(GraphFeed::instance(), &GraphFeed::changed, &m_refreshSoon, qOverload<>(&QTimer::start));
}

QObject *ChronologyBrowser::backend() const { return m_backend; }

void ChronologyBrowser::setBackend(QObject *backend) {
    auto *given = qobject_cast<Backend *>(backend);
    if (given == m_backend)
        return;
    if (m_backend)
        m_backend->disconnect(this);
    m_backend = given;
    if (m_backend) {
        connect(m_backend, &Backend::revisionChanged, &m_refreshSoon, qOverload<>(&QTimer::start));
        connect(m_backend, &Backend::libraryChanged, this, [this] { showScope({}); });
        connect(m_backend, &QObject::destroyed, this, [this] { m_backend = nullptr; });
    }
    emit backendChanged();
}

void ChronologyBrowser::setLibrary(const Library *library) { m_library = library; }

const Library *ChronologyBrowser::library() const { return m_backend ? m_backend->library() : m_library; }

QString ChronologyBrowser::showing() const {
    switch (m_from.what) {
    case From::What::Book:
        return u"book"_s;
    case From::What::Series:
        return u"series"_s;
    case From::What::Collection:
        return u"collection"_s;
    case From::What::Scope:
        break;
    }
    return u"scope"_s;
}

void ChronologyBrowser::showScope(const QString &scope) { open({.what = From::What::Scope, .scope = scope, .bookId = 0, .name = {}}); }
void ChronologyBrowser::showBook(qint64 bookId) { open({.what = From::What::Book, .scope = {}, .bookId = bookId, .name = {}}); }
void ChronologyBrowser::showSeries(const QString &series) { open({.what = From::What::Series, .scope = {}, .bookId = 0, .name = series}); }
void ChronologyBrowser::showCollection(const QString &collection) {
    open({.what = From::What::Collection, .scope = {}, .bookId = 0, .name = collection});
}

void ChronologyBrowser::open(From from) {
    m_from = std::move(from);
    m_lanes.clear();
    m_books.clear();
    m_involved.clear();
    m_series.clear();
    m_onlyMet = false;
    m_rangeFrom.clear();
    m_rangeTo.clear();
    m_selected = 0;
    m_mark = -1;
    load();
    // What it was opened from is its books' filter, one chip to take off.
    if (m_from.what == From::What::Book)
        m_books.insert(m_from.bookId);
    else if (m_from.what != From::What::Scope)
        m_series.insert((m_from.what == From::What::Series ? u"series:"_s : u"collection:"_s) + m_from.name);
    filter();
    emit changed();
}

void ChronologyBrowser::refresh() {
    load();
    filter();
    emit changed();
}

void ChronologyBrowser::load() {
    const Library *books = library();
    m_read = books ? Reading::read(*books, m_from) : Reading::Read{};
    m_chart = Chart::chart(m_read.parts);
    m_title.clear();
    if (books && m_from.what == From::What::Book)
        m_title = books->book(m_from.bookId).transform([](const Book &book) { return book.meta.title; }).value_or(QString());
    else if (m_from.what != From::What::Scope)
        m_title = m_from.name;
    m_groups.clear();
    if (books)
        for (const Book &book : books->books()) {
            if (!book.meta.series.isEmpty())
                m_groups[u"series:"_s + book.meta.series].insert(book.id);
            for (const Collection &collection : books->collectionsOf(book))
                m_groups[u"collection:"_s + collection.name].insert(book.id);
        }
    m_here = findHere();
    if (!m_chart.items.contains(m_selected))
        m_selected = 0;
    describe();
    emit selectedChanged();
}

QString ChronologyBrowser::laneKey(int lane) const {
    const GraphNode &timeline = m_chart.lanes.at(lane).timeline;
    return timeline.id != 0 ? u"t:"_s + QString::number(timeline.id) : u"b:"_s + QString::number(timeline.bookId);
}

bool ChronologyBrowser::passesEvent(qint64 event) const {
    const Reading::Event found = m_read.events.value(event);
    if (m_onlyMet && found.books.isEmpty())
        return false;
    if (!m_involved.isEmpty() && std::ranges::none_of(found.involved, [this](qint64 thing) { return m_involved.contains(thing); }))
        return false;
    if (m_books.isEmpty() && m_series.isEmpty())
        return true;
    return found.books.intersects(m_books)
           || std::ranges::any_of(m_series, [&](const QString &group) { return found.books.intersects(m_groups.value(group)); });
}

bool ChronologyBrowser::passes(const Chart::Mark &mark) const {
    if (!m_lanes.isEmpty() && !m_lanes.contains(laneKey(mark.lane)))
        return false;
    if (!m_rangeFrom.isEmpty() || !m_rangeTo.isEmpty()) {
        const Chart::Band &band = m_chart.bands.at(m_chart.lanes.at(mark.lane).band);
        const auto stretch = stretchOn(band, m_rangeFrom, m_rangeTo);
        if (!stretch || (stretch->to && band.value(mark.from) >= *stretch->to) || (stretch->from && band.value(mark.to) < *stretch->from))
            return false;
    }
    return passesEvent(mark.event);
}

void ChronologyBrowser::filter() {
    m_passes.resize(m_chart.marks.size());
    QSet<qint64> shown;
    for (const auto [at, mark] : std::views::enumerate(m_chart.marks)) {
        m_passes[at] = passes(mark);
        if (m_passes.at(at))
            shown.insert(mark.event);
    }
    m_shownCount = static_cast<int>(shown.size());
}

QVariantList ChronologyBrowser::unplaced() const {
    QVariantList listed;
    if (!m_showUnplaced)
        return listed;
    for (const Item &item : m_chart.unplaced)
        if (passesEvent(item.event.id))
            listed << QVariantMap{{"id", item.event.id},
                                  {"name", item.event.name},
                                  {"said", item.time ? item.time->time.said : QString()},
                                  {"from", fromOf(item.event.id)}};
    return listed;
}

void ChronologyBrowser::setShowUnplaced(bool show) {
    if (show == m_showUnplaced)
        return;
    m_showUnplaced = show;
    emit changed();
}

QString ChronologyBrowser::nameOf(qint64 event) const { return m_read.events.value(event).node.name; }
QString ChronologyBrowser::fromOf(qint64 event) const { return m_read.events.value(event).from; }

QString ChronologyBrowser::laneName(int lane) const {
    const GraphNode &timeline = m_chart.lanes.at(lane).timeline;
    if (timeline.id != 0)
        return timeline.name;
    const auto book = timeline.bookId > 0 && library() ? library()->book(timeline.bookId) : std::nullopt;
    return book ? book->meta.title : tr("Of no book");
}

QString ChronologyBrowser::bandName(int band) const {
    const Chart::Band &of = m_chart.bands.at(band);
    QString named = tr("In the order they happen; no dates are known");
    if (of.dated())
        named = of.calendar == EventTime::gregorianName() ? tr("By date") : tr("By the calendar of %1").arg(of.calendar);
    const QString part = m_read.parts.value(of.part).name;
    return part.isEmpty() ? named : tr("%1: %2", "a part of the library, and how its events are ordered").arg(part, named);
}

QString ChronologyBrowser::tickText(const QString &calendar, const EventTime::Point &point) {
    return ChronologyCard::point(calendar, point);
}

qint64 ChronologyBrowser::findHere() const {
    const Library *books = library();
    if (!books)
        return 0;
    // The book being read of those shown, the one read last.
    std::optional<Book> reading;
    for (const Book &book : books->books())
        if (book.status() == "reading"_L1 && (m_read.books.isEmpty() || m_read.books.contains(book.id))
            && (!reading || book.lastRead > reading->lastRead))
            reading = book;
    if (!reading)
        return 0;
    // Of what it was read up to, the event met last.
    qint64 here = 0;
    std::optional<Revealed> latest;
    for (auto event = m_read.events.cbegin(); event != m_read.events.cend(); ++event)
        for (const GraphPlace &place : event->met)
            if (place.bookId == reading->id && m_chart.marksOf.contains(event.key()) && (!latest || revealed(place) > *latest)) {
                latest = revealed(place);
                here = event.key();
            }
    return here;
}

QVariantList ChronologyBrowser::filters() const {
    QVariantList chips;
    const Library *books = library();
    for (const qint64 book : m_books)
        chips << QVariantMap{
            {"act", u"book"_s},
            {"value", book},
            {"label", books ? books->book(book).transform([](const Book &b) { return b.meta.title; }).value_or(QString()) : QString()}};
    for (const QString &group : m_series)
        chips << QVariantMap{{"act", u"series"_s}, {"value", group}, {"label", group.section(u':', 1)}};
    for (const auto [at, lane] : std::views::enumerate(m_chart.lanes))
        if (m_lanes.contains(laneKey(static_cast<int>(at))))
            chips << QVariantMap{
                {"act", u"timeline"_s}, {"value", laneKey(static_cast<int>(at))}, {"label", laneName(static_cast<int>(at))}};
    for (const qint64 thing : m_involved)
        chips << QVariantMap{{"act", u"involved"_s}, {"value", thing}, {"label", m_read.involved.value(thing).name}};
    if (m_onlyMet)
        chips << QVariantMap{{"act", u"met"_s}, {"value", true}, {"label", tr("Only met in a book")}};
    if (!m_rangeFrom.isEmpty() || !m_rangeTo.isEmpty())
        chips << QVariantMap{{"act", u"range"_s}, {"value", QString()}, {"label", rangeName()}};
    return chips;
}

QString ChronologyBrowser::rangeName() const {
    if (m_rangeTo.isEmpty())
        return tr("From %1").arg(m_rangeFrom);
    if (m_rangeFrom.isEmpty())
        return tr("Until %1").arg(m_rangeTo);
    return tr("%1 to %2", "a stretch of time").arg(m_rangeFrom, m_rangeTo);
}

QVariantList ChronologyBrowser::choices() const {
    QVariantList list;
    const auto choice = [&list](const QString &kindName, const QString &label, const QString &act, const QVariant &value, bool on,
                                int count, bool usual, const QString &mark = {}) {
        list << QVariantMap{{"kindName", kindName}, {"label", label},  {"act", act},  {"value", value}, {"on", on},
                            {"count", count},       {"common", usual}, {"mark", mark}};
    };
    if (!filters().isEmpty())
        choice({}, tr("Show everything again"), u"clear"_s, {}, false, 0, true);
    choice({}, tr("Only the events met in a book, not those only implied"), u"met"_s, true, m_onlyMet, 0, true);
    choice({}, tr("The events of which no time or order is known"), u"unplaced"_s, true, m_showUnplaced,
           static_cast<int>(m_chart.unplaced.size()), true);
    for (const auto [at, lane] : std::views::enumerate(m_chart.lanes)) {
        const int count = static_cast<int>(std::ranges::count(m_chart.marks, static_cast<int>(at), &Chart::Mark::lane));
        choice(tr("Timeline"), laneName(static_cast<int>(at)), u"timeline"_s, laneKey(static_cast<int>(at)),
               m_lanes.contains(laneKey(static_cast<int>(at))), count, true);
    }
    QMap<qint64, int> books;
    QHash<qint64, int> things;
    for (const Reading::Event &event : m_read.events) {
        for (const qint64 book : event.books)
            ++books[book];
        for (const qint64 thing : event.involved)
            ++things[thing];
    }
    const Library *library = this->library();
    for (auto book = books.cbegin(); book != books.cend(); ++book)
        if (const auto held = library ? library->book(book.key()) : std::nullopt)
            choice(tr("Book"), held->meta.title, u"book"_s, book.key(), m_books.contains(book.key()), *book, books.size() <= common);
    for (auto group = m_groups.cbegin(); group != m_groups.cend(); ++group)
        if (std::ranges::any_of(*group, [&books](qint64 book) { return books.contains(book); }))
            choice(group.key().startsWith(u"series:"_s) ? tr("Series") : tr("Collection"), group.key().section(u':', 1), u"series"_s,
                   group.key(), m_series.contains(group.key()), 0, true);
    QList<qint64> involved = things.keys();
    std::ranges::sort(involved, std::greater{}, [&things](qint64 thing) { return std::pair{things.value(thing), -thing}; });
    for (const auto [at, thing] : std::views::enumerate(involved)) {
        const GraphNode node = m_read.involved.value(thing);
        choice({}, node.name, u"involved"_s, thing, m_involved.contains(thing), things.value(thing), at < common, node.kind);
    }
    return list;
}

void ChronologyBrowser::setFilter(const QString &act, const QVariant &value, bool on) {
    const auto toggle = [on](auto &set, const auto &key) {
        if (on)
            set.insert(key);
        else
            set.remove(key);
    };
    if (act == "clear"_L1) {
        clearFilters();
        return;
    }
    if (act == "unplaced"_L1) {
        setShowUnplaced(on);
        return;
    }
    if (act == "timeline"_L1) {
        toggle(m_lanes, value.toString());
    } else if (act == "book"_L1) {
        toggle(m_books, value.toLongLong());
    } else if (act == "series"_L1) {
        toggle(m_series, value.toString());
    } else if (act == "involved"_L1) {
        toggle(m_involved, value.toLongLong());
    } else if (act == "met"_L1) {
        m_onlyMet = on;
    } else if (act == "range"_L1 && !on) {
        m_rangeFrom.clear();
        m_rangeTo.clear();
    }
    filter();
    emit changed();
}

bool ChronologyBrowser::setRange(const QString &from, const QString &to) {
    const QString start = from.trimmed();
    const QString end = to.trimmed();
    if (!parses(start) || !parses(end))
        return false;
    m_rangeFrom = start;
    m_rangeTo = end;
    filter();
    emit changed();
    return true;
}

void ChronologyBrowser::clearFilters() {
    m_lanes.clear();
    m_books.clear();
    m_series.clear();
    m_involved.clear();
    m_onlyMet = false;
    m_rangeFrom.clear();
    m_rangeTo.clear();
    filter();
    emit changed();
}

void ChronologyBrowser::select(qint64 event) {
    if (event == m_selected)
        return;
    m_selected = m_chart.items.contains(event) ? event : 0;
    const QList<int> marks = m_chart.marksOf.value(m_selected);
    if (!marks.contains(m_mark))
        m_mark = marks.isEmpty() ? -1 : marks.first();
    describe();
    emit selectedChanged();
}

bool ChronologyBrowser::shownMark(int mark) const { return mark >= 0 && mark < m_passes.size() && m_passes.at(mark); }

void ChronologyBrowser::takeMark(int mark) {
    m_mark = mark;
    m_selected = m_chart.marks.at(mark).event;
    describe();
    emit selectedChanged();
}

int ChronologyBrowser::along(int by) const {
    const int lane = m_chart.marks.at(m_mark).lane;
    for (int next = m_mark + by; next >= 0 && next < m_chart.marks.size() && m_chart.marks.at(next).lane == lane; next += by)
        if (shownMark(next))
            return next;
    return -1;
}

int ChronologyBrowser::across(int by) const {
    const auto middle = [this](int mark) { return (m_chart.marks.at(mark).from + m_chart.marks.at(mark).to) / 2; };
    const double here = middle(m_mark);
    for (int lane = m_chart.marks.at(m_mark).lane + by; lane >= 0 && lane < m_chart.lanes.size(); lane += by) {
        int best = -1;
        for (const auto [index, mark] : std::views::enumerate(m_chart.marks))
            if (mark.lane == lane && shownMark(static_cast<int>(index))
                && (best < 0 || std::abs(middle(static_cast<int>(index)) - here) < std::abs(middle(best) - here)))
                best = static_cast<int>(index);
        if (best >= 0)
            return best;
    }
    return -1;
}

void ChronologyBrowser::step(const QString &direction) {
    if (!shownMark(m_mark)) {
        if (const auto first = std::ranges::find(m_passes, true); first != m_passes.end())
            takeMark(static_cast<int>(first - m_passes.begin()));
        return;
    }
    const bool sideways = direction == "left"_L1 || direction == "right"_L1;
    const int by = direction == "right"_L1 || direction == "down"_L1 ? 1 : -1;
    // The next along the lane, or the nearest on the next lane that way with anything shown.
    if (const int next = sideways ? along(by) : across(by); next >= 0)
        takeMark(next);
}

QVariantList ChronologyBrowser::find(const QString &words) const {
    const QStringList wanted = words.toLower().split(u' ', Qt::SkipEmptyParts);
    QVariantList found;
    QSet<qint64> once;
    const auto goesBy = [&wanted](const GraphNode &node) {
        const QString names = (node.name + u' ' + node.aliases().join(u' ')).toLower();
        return std::ranges::all_of(wanted, [&names](const QString &word) { return names.contains(word); });
    };
    for (const auto [at, mark] : std::views::enumerate(m_chart.marks)) {
        const GraphNode node = m_read.events.value(mark.event).node;
        if (m_passes.value(at) && !once.contains(mark.event) && goesBy(node) && found.size() < 50) {
            once.insert(mark.event);
            found << QVariantMap{{"id", node.id}, {"name", node.name}, {"detail", laneName(mark.lane)}};
        }
    }
    return found;
}

void ChronologyBrowser::describe() {
    const Library *books = library();
    m_details = books && m_selected != 0
                    ? ChronologyCard::of(*books, m_read, m_chart, m_selected, [this](int lane) { return laneName(lane); })
                    : QVariantMap{};
}
