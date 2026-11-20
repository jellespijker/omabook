#include "chronologycard.h"

#include <QCoreApplication>
#include <QDate>
#include <QLocale>
#include <QTime>
#include <QVariantList>
#include <algorithm>

#include "library.h"

using namespace Chronology;
using namespace Qt::StringLiterals;

namespace {

// In the catalogs: bin/i18n reads what tr() is handed.
const auto tr = [](const char *text, const char *disambiguation = nullptr) {
    return QCoreApplication::translate("ChronologyCard", text, disambiguation);
};

[[nodiscard]] QString year(qint64 year) {
    // Astronomical years: 0 is 1 BC.
    return year > 0 ? QString::number(year) : tr("%1 BC").arg(1 - year);
}

[[nodiscard]] QString kindOf(Contradiction::Kind kind) {
    switch (kind) {
    case Contradiction::Kind::Cycle:
        return tr("What comes first goes round in a circle");
    case Contradiction::Kind::Date:
        return tr("An order that its dates say can't be");
    case Contradiction::Kind::Times:
        return tr("Two books give it times that can't both be");
    case Contradiction::Kind::Calendars:
        return tr("Two calendars are tied together two ways");
    }
    return {};
}

[[nodiscard]] QString ruleOf(Contradiction::Kind kind) {
    switch (kind) {
    case Contradiction::Kind::Cycle:
        return tr("The later revealed is followed, as a flashback corrects what was thought before.");
    case Contradiction::Kind::Date:
        return tr("The dates are followed, and the order is set aside.");
    case Contradiction::Kind::Times:
        return tr("One time is followed: what you put in, else the more precise, else the first book's.");
    case Contradiction::Kind::Calendars:
        return tr("The first revealed is followed.");
    }
    return {};
}

[[nodiscard]] QVariantMap sourceOf(const Library &library, const Source &source) {
    // A time is said as one, not as it is kept.
    QString said = source.said;
    if (source.fact != 0)
        if (const auto time = EventTime::parse(QStringView(source.said)); time)
            said = ChronologyCard::when(*time);
    return {{"said", said}, {"where", ChronologyCard::where(library, source.revealed)}};
}

[[nodiscard]] QVariantList contradictionsOf(const Library &library, const Chart::Chart &chart, qint64 event) {
    QVariantList told;
    for (const Contradiction &contradiction : Chart::contradictionsOf(chart, event)) {
        QVariantList sources;
        for (const Source &source : contradiction.sources)
            sources << sourceOf(library, source);
        told << QVariantMap{{"kind", kindOf(contradiction.kind)},
                            {"rule", ruleOf(contradiction.kind)},
                            {"sources", sources},
                            {"setAside", sourceOf(library, contradiction.setAside).value("said")}};
    }
    return told;
}

// Where to go back to on the page: where its time was revealed, else where it was first met.
[[nodiscard]] QVariantMap pageOf(const Library &library, const Reading::Event &event, const Item &item) {
    if (item.time && item.time->revealed.place.bookId > 0)
        return ChronologyCard::where(library, item.time->revealed);
    if (event.met.isEmpty())
        return {};
    const GraphPlace first = *std::ranges::min_element(event.met, {}, [](const GraphPlace &place) { return revealed(place); });
    const auto moment = event.node.id > 0 ? Graph::ofReader(library, first.bookId).firstMet(event.node.id, first.bookId) : std::nullopt;
    return ChronologyCard::where(library, moment.value_or(GraphMoment{.place = first, .anchor = {}}));
}

} // namespace

QString ChronologyCard::point(const QString &calendar, const EventTime::Point &point) {
    // A story's own calendar as the book writes it, the time of day apart: "301-03-29 12:00".
    if (calendar != EventTime::gregorianName()) {
        EventTime::Point day = point;
        day.hour.reset();
        day.minute.reset();
        return day.text()
               + (point.hour ? u' ' + QString::number(*point.hour).rightJustified(2, u'0') + u':'
                                   + QString::number(point.minute.value_or(0)).rightJustified(2, u'0')
                             : QString());
    }
    const QLocale locale;
    const QString month = point.month ? locale.standaloneMonthName(*point.month) : QString();
    QString said = year(point.year);
    if (point.day)
        said = tr("%1 %2 %3", "a day, its month and its year").arg(QString::number(*point.day), month, year(point.year));
    else if (point.month)
        said = tr("%1 %2", "a month and its year").arg(month, year(point.year));
    if (point.hour)
        said += u", "_s + QTime(*point.hour, point.minute.value_or(0)).toString(locale.timeFormat(QLocale::ShortFormat));
    return said;
}

QString ChronologyCard::when(const EventTime &time) {
    const QString calendar = time.on.calendar;
    // "Around" goes with each moment: "from around 1941".
    const auto said = [&](const std::optional<EventTime::Point> &at) {
        if (!at)
            return QString();
        return time.approximate ? tr("around %1").arg(point(calendar, *at)) : point(calendar, *at);
    };
    QString told;
    if (time.on.at)
        told = said(time.on.at);
    else if (time.on.from && time.on.to)
        told = tr("%1 – %2", "a span of time").arg(said(time.on.from), said(time.on.to));
    else if (time.on.from)
        told = tr("from %1").arg(said(time.on.from));
    else if (time.on.to)
        told = tr("until %1").arg(said(time.on.to));
    if (told.isEmpty())
        return time.said;
    return time.on.gregorian() ? told : tr("%1, %2", "a time and the calendar it is on").arg(told, calendar);
}

QVariantMap ChronologyCard::where(const Library &library, const GraphMoment &moment) {
    if (moment.place.bookId <= 0)
        return {};
    const auto book = library.book(moment.place.bookId);
    return {{"bookId", moment.place.bookId},
            {"section", moment.place.section},
            {"fraction", moment.place.fraction},
            {"anchor", moment.anchor},
            {"title", book ? book->meta.title : QString()},
            {"readable", book && !book->formats.isEmpty()}};
}

QVariantMap ChronologyCard::of(const Library &library, const Reading::Read &read, const Chart::Chart &chart, qint64 event,
                               const std::function<QString(int)> &laneName) {
    const auto found = read.events.constFind(event);
    if (found == read.events.cend())
        return {};
    const Item item = chart.items.value(event);
    QStringList timelines;
    for (const int mark : chart.marksOf.value(event))
        timelines << laneName(chart.marks.at(mark).lane);
    QVariantList involved;
    for (const qint64 thing : found->involved) {
        const GraphNode node = read.involved.value(thing);
        involved << QVariantMap{{"id", node.id}, {"name", node.name}, {"kind", node.kind}};
    }
    QVariantList books;
    for (const GraphPlace &place : found->met)
        if (const QVariantMap book = where(library, {.place = place, .anchor = {}}); !book.isEmpty())
            books << book;
    const QVariantList contradictions = contradictionsOf(library, chart, event);
    const GraphPlace firstBook = books.isEmpty() ? GraphPlace{} : found->met.first();
    return {{"id", event},
            {"name", found->node.name},
            {"summary", found->node.summary},
            {"from", found->from},
            {"when", item.time ? when(item.time->time) : QString()},
            {"said", item.time ? item.time->time.said : QString()},
            {"timelines", timelines},
            {"during", item.within != 0 ? read.events.value(item.within).node.name : QString()},
            {"involved", involved},
            {"books", books},
            {"contradictions", contradictions},
            {"page", pageOf(library, *found, item)},
            {"graph", QVariantMap{{"node", event}, {"bookId", firstBook.bookId}}}};
}
