// The chronology in order (#43): of each timeline, its events in the order
// they happen, from their times, their relations of order and the timelines
// they are on, with the contradictions said and set aside by the rule; the
// timelines lined up on shared axes; never ahead of the reader.

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <ranges>

#include "calendars.h"
#include "chronicle.h"
#include "chronology.h"
#include "chronologyorder.h"
#include "eventtime.h"
#include "graph.h"
#include "graphcontext.h"
#include "library.h"

using Chronology::Contradiction;
using Chronology::Lane;

namespace {

[[nodiscard]] EventTime timeOf(const QString &json) {
    const auto time = EventTime::parse(json);
    if (!time)
        qFatal("%s", qPrintable(time.error()));
    return *time;
}

[[nodiscard]] QString year(qint64 year) { return QString(R"({"calendar": "gregorian", "at": "%1"})").arg(year); }

// Facts made by hand, as a chronicle is made from them.
struct Told {
    Chronology::Facts facts;
    qint64 next = 1;

    qint64 event(const QString &name, const GraphPlace &met = {1, 0, 0}) {
        const qint64 id = next++;
        GraphNode node;
        node.id = id;
        node.kind = "event";
        node.name = name;
        facts.events.append(node);
        facts.met.insert(id, {met});
        return id;
    }
    qint64 timeline(const QString &name) {
        const qint64 id = next++;
        GraphNode node;
        node.id = id;
        node.kind = Chronology::timelineKind();
        node.name = name;
        facts.timelines.append(node);
        return id;
    }
    qint64 when(qint64 event, const QString &json, const GraphPlace &at = {1, 0, 0}) {
        const qint64 fact = next++;
        facts.times[event].append({.time = timeOf(json), .revealed = {at, {}}, .fact = fact});
        return fact;
    }
    qint64 relate(qint64 event, const QString &relation, qint64 other, const GraphPlace &at = {1, 0, 0}) {
        const qint64 edge = next++;
        facts.edges.append({.id = edge, .source = event, .target = other, .kind = relation, .note = {}, .learned = at, .anchor = {}});
        return edge;
    }
    void on(qint64 event, qint64 timeline) {
        facts.edges.append(
            {.id = next++, .source = event, .target = timeline, .kind = "on", .note = {}, .learned = {1, 0, 0}, .anchor = {}});
    }
};

// An order as "A < B=C < D": events at the same time joined by "=".
[[nodiscard]] QString order(const Lane &lane) {
    QStringList ranks;
    int rank = -1;
    for (const Chronology::Item &item : lane.items) {
        if (item.rank != rank)
            ranks.append(item.event.name);
        else
            ranks.last() += "=" + item.event.name;
        rank = item.rank;
    }
    return ranks.join(" < ");
}

[[nodiscard]] const Lane &laneOf(const Chronology::Chronicle &chronicle, const char *timeline) {
    const auto found = std::ranges::find(chronicle.lanes, timeline, [](const Lane &lane) { return lane.timeline.name; });
    if (found == chronicle.lanes.cend())
        qFatal("no lane %s", timeline);
    return *found;
}

[[nodiscard]] const Chronology::Item &itemOf(const Lane &lane, const char *name) {
    const auto found = std::ranges::find(lane.items, name, [](const Chronology::Item &item) { return item.event.name; });
    if (found == lane.items.cend())
        qFatal("no item %s", name);
    return *found;
}

[[nodiscard]] QByteArray written(const Chronology::Chronicle &chronicle) {
    return QJsonDocument(chronicle.toJson()).toJson(QJsonDocument::Compact);
}

// The same facts in another order: the events, the times and the relations.
[[nodiscard]] Chronology::Facts shuffled(Chronology::Facts facts, QRandomGenerator &random) {
    const auto shuffle = [&random](auto &list) {
        for (qsizetype at = list.size() - 1; at > 0; --at)
            list.swapItemsAt(at, random.bounded(at + 1));
    };
    shuffle(facts.events);
    shuffle(facts.timelines);
    shuffle(facts.edges);
    return facts;
}

// A timeline of `count` events, each at a time of the story: most with its
// year (some to the day), the rest with none. The relations agree with those
// times, apart from a few that contradict them.
[[nodiscard]] Told many(int count, quint32 seed) {
    Told told;
    QRandomGenerator random(seed);
    const qint64 line = told.timeline("Long");
    QList<qint64> events;
    QHash<qint64, int> happens;
    for (int at = 0; at < count; ++at) {
        const qint64 event = told.event(QString("E%1").arg(at), {1 + (at % 5), at, 0.5});
        const int when = random.bounded(1, 5000);
        told.on(event, line);
        events.append(event);
        happens.insert(event, when);
        if (random.bounded(3) != 0)
            told.when(event, at % 7 == 0 ? QString(R"({"calendar": "gregorian", "at": "%1-03-04"})").arg(when) : year(when));
    }
    for (int at = 0; at < count * 2; ++at) {
        const qint64 one = events.at(random.bounded(count)), other = events.at(random.bounded(count));
        if (one == other || happens.value(one) == happens.value(other))
            continue;
        const bool agrees = (happens.value(one) < happens.value(other)) != (at % 97 == 0);
        const qint64 first = agrees ? one : other, second = agrees ? other : one;
        if (at % 11 == 0)
            told.relate(second, "after", first, {1, at % 40, 0.5});
        else
            told.relate(first, "before", second, {1, at % 40, 0.5});
    }
    return told;
}

struct Reading {
    Reading() : library(dir.filePath("Library")) {
        QVERIFY(library.open());
        BookMeta meta;
        meta.title = "The Long Siege";
        book = library.addEntry(meta, true).id;
        QVERIFY(book > 0);
    }
    void readTo(int section) { library.setProgress(book, {section, 0.9, section * 10.0}); }
    [[nodiscard]] Chronology::Chronicle chronicle() const {
        return Chronology::chronicle(Chronology::read(library, GraphContext::readerKnows(library), {}));
    }

    QTemporaryDir dir;
    Library library;
    qint64 book = 0;
};

} // namespace

class ChronicleTests : public QObject {
    Q_OBJECT

private slots:
    void datesPlaceEventsOnTheirCalendar() {
        Told told;
        const qint64 war = told.timeline("The war");
        const qint64 landing = told.event("Landing"), invasion = told.event("Invasion"), surrender = told.event("Surrender");
        told.when(landing, R"({"calendar": "gregorian", "at": "1944-06-06"})");
        told.when(invasion, R"({"calendar": "gregorian", "at": "1939-09-01"})");
        told.when(surrender, R"({"calendar": "gregorian", "at": "1945-05"})");
        for (const qint64 event : {landing, invasion, surrender})
            told.on(event, war);
        const auto chronicle = Chronology::chronicle(told.facts);
        QCOMPARE(chronicle.lanes.size(), 1);
        const Lane &lane = laneOf(chronicle, "The war");
        QCOMPARE(order(lane), QString("Invasion < Landing < Surrender"));
        QCOMPARE(lane.calendar, EventTime::gregorianName());
        // Each where it is: a day lasts a day, a month to the start of the next.
        const auto &day = *itemOf(lane, "Landing").extent;
        QCOMPARE(*day.to - *day.from, 24 * 60);
        const auto &month = *itemOf(lane, "Surrender").extent;
        QCOMPARE(*month.to - *month.from, 31 * 24 * 60);
        QVERIFY(lane.contradictions.isEmpty() && chronicle.unplaced.isEmpty());
    }

    void relationsAloneMakeAnOrder() {
        Told told;
        const qint64 d = told.event("D"), b = told.event("B"), a = told.event("A"), c = told.event("C"), e = told.event("E");
        told.relate(d, "after", c);
        told.relate(b, "after", a);
        told.relate(c, "after", b);
        told.relate(e, "same time as", c);
        const auto chronicle = Chronology::chronicle(told.facts);
        // On no timeline, they are their book's own order.
        QCOMPARE(chronicle.lanes.size(), 1);
        QCOMPARE(chronicle.lanes.first().timeline.id, 0);
        QCOMPARE(chronicle.lanes.first().timeline.bookId, 1);
        QCOMPARE(order(chronicle.lanes.first()), QString("A < B < C=E < D"));
        QVERIFY(chronicle.lanes.first().calendar.isEmpty());
    }

    void datesAndRelationsTogether() {
        Told told;
        const qint64 line = told.timeline("Line");
        const qint64 a = told.event("A"), b = told.event("B"), c = told.event("C"), d = told.event("D"), e = told.event("E");
        const qint64 year44 = told.event("Year"), june = told.event("June");
        told.when(a, year(1940));
        told.when(c, year(1950));
        told.relate(b, "after", a);
        told.relate(b, "before", c);
        told.relate(d, "after", c);
        told.relate(e, "before", a);
        // Times that overlap are ordered by what the book says of them.
        told.when(year44, year(1944));
        told.when(june, R"({"calendar": "gregorian", "at": "1944-06-06"})");
        told.relate(june, "before", year44);
        for (const qint64 event : {a, b, c, d, e, year44, june})
            told.on(event, line);
        const auto chronicle = Chronology::chronicle(told.facts);
        QCOMPARE(order(laneOf(chronicle, "Line")), QString("E < A < B < June < Year < C < D"));
        QVERIFY(!itemOf(laneOf(chronicle, "Line"), "B").extent);
    }

    void spansAndDuring() {
        Told told;
        const qint64 line = told.timeline("Line");
        const qint64 war = told.event("War"), siege = told.event("Siege"), rumour = told.event("Rumour"), peace = told.event("Peace");
        told.when(war, R"({"calendar": "gregorian", "from": "1939-09-01", "to": "1945-09-02"})");
        told.when(siege, R"({"calendar": "gregorian", "at": "1944-06"})");
        told.when(peace, year(1946));
        told.relate(rumour, "during", war);
        // Of no time, a campaign that comes before what follows it, met first.
        const qint64 aftermath = told.event("Aftermath", {1, 0, 0.1});
        const qint64 campaign = told.event("Campaign", {1, 2, 0});
        const qint64 battle = told.event("Battle", {1, 3, 0});
        told.relate(battle, "during", campaign);
        told.relate(campaign, "before", aftermath);
        for (const qint64 event : {war, siege, rumour, peace, aftermath, campaign, battle})
            told.on(event, line);
        const auto chronicle = Chronology::chronicle(told.facts);
        const Lane &lane = laneOf(chronicle, "Line");
        QCOMPARE(order(lane), QString("Campaign < Battle < Aftermath < War < Rumour < Siege < Peace"));
        QCOMPARE(itemOf(lane, "Rumour").within, war);
        QCOMPARE(itemOf(lane, "Battle").within, campaign);
        const auto &span = *itemOf(lane, "War").extent;
        QVERIFY(span.from && span.to && span.from < itemOf(lane, "Siege").extent->from && span.to < itemOf(lane, "Peace").extent->from);
        // A span with an open end runs on.
        told.when(peace, R"({"calendar": "gregorian", "from": "1946"})");
        QVERIFY(!itemOf(laneOf(Chronology::chronicle(told.facts), "Line"), "Peace").extent->to);
        QVERIFY(lane.contradictions.isEmpty());
    }

    void calendarsTiedByTheSameTime() {
        Told told;
        const qint64 line = told.timeline("Middle-earth");
        const qint64 ring = told.event("Ring"), party = told.event("Party"), crowning = told.event("Crowning");
        const qint64 night = told.event("Night");
        told.when(ring, R"({"calendar": "Third Age", "at": "3019-03-25", "also": [{"calendar": "Shire Reckoning", "at": "1419-03-25"}]})");
        told.when(party, R"({"calendar": "Shire Reckoning", "at": "1418"})");
        told.when(crowning, R"({"calendar": "Third Age", "at": "3020"})");
        told.when(night, R"({"calendar": "The Long Night", "at": "3"})"); // tied to nothing
        told.relate(night, "before", party);
        for (const qint64 event : {ring, party, crowning, night})
            told.on(event, line);
        const auto chronicle = Chronology::chronicle(told.facts);
        const Lane &lane = laneOf(chronicle, "Middle-earth");
        // Both reach all three; at a tie the calendars go by name.
        QCOMPARE(lane.calendar, QString("Shire Reckoning"));
        QCOMPARE(order(lane), QString("Night < Party < Ring < Crowning"));
        QVERIFY(!itemOf(lane, "Night").extent);

        // Put on another calendar to the day, where the tie is alike to the day.
        const Chronology::Calendars calendars(told.facts.times.value(ring));
        const auto shire = calendars.scale("Shire Reckoning");
        QCOMPARE(shire.on(timeOf(R"({"calendar": "Third Age", "at": "3019-03-26"})"))->at->text(), QString("1419-03-26"));
        QVERIFY(calendars.tied("Third Age", "Shire Reckoning") && !calendars.tied("Third Age", "The Long Night"));
        // Two ties that disagree: the first is kept, and the other is said.
        const qint64 other = told.event("Other");
        told.when(other, R"({"calendar": "Third Age", "at": "3000", "also": [{"calendar": "Shire Reckoning", "at": "1500"}]})", {2, 0, 0});
        const auto disagreeing = Chronology::chronicle(told.facts);
        QCOMPARE(disagreeing.contradictions.size(), 1);
        QCOMPARE(disagreeing.contradictions.first().kind, Contradiction::Kind::Calendars);
        QCOMPARE(disagreeing.contradictions.first().setAside.event, other);
        QCOMPARE(order(laneOf(disagreeing, "Middle-earth")), QString("Night < Party < Ring < Crowning"));
    }

    void relationsInACircleGiveWayToTheLaterRevealed() {
        Told told;
        const qint64 a = told.event("A"), b = told.event("B"), c = told.event("C");
        const qint64 first = told.relate(a, "before", b, {1, 1, 0});
        told.relate(b, "before", c, {1, 2, 0});
        told.relate(c, "before", a, {1, 3, 0});
        const auto chronicle = Chronology::chronicle(told.facts);
        const Lane &lane = chronicle.lanes.first();
        QCOMPARE(order(lane), QString("B < C < A"));
        QCOMPARE(lane.contradictions.size(), 1);
        const Contradiction &circle = lane.contradictions.first();
        QCOMPARE(circle.kind, Contradiction::Kind::Cycle);
        QCOMPARE(circle.events, QList<qint64>({a, b, c}));
        QCOMPARE(circle.sources.size(), 3);
        QCOMPARE(circle.sources.first().revealed.place.section, 1);
        QCOMPARE(circle.setAside.edge, first);
        QCOMPARE(circle.setAside.said, QString("A before B"));
        QVERIFY(!circle.rule().isEmpty());
        // What no book says, put in by hand, counts as revealed last.
        told.relate(a, "after", c, {0, 0, 0});
        QCOMPARE(order(Chronology::chronicle(told.facts).lanes.first()), QString("B < C < A"));
    }

    void datesWinOverRelations() {
        Told told;
        const qint64 x = told.event("X"), y = told.event("Y");
        told.when(x, year(1950));
        told.when(y, year(1940));
        const qint64 wrong = told.relate(x, "before", y);
        // And through a chain of relations.
        const qint64 p = told.event("P"), q = told.event("Q"), r = told.event("R");
        told.when(p, year(2050));
        told.when(r, year(2040));
        const qint64 earlier = told.relate(p, "before", q, {1, 1, 0});
        told.relate(q, "before", r, {1, 2, 0});
        const auto chronicle = Chronology::chronicle(told.facts);
        const Lane &lane = chronicle.lanes.first();
        QCOMPARE(order(lane), QString("Y < X < Q < R < P"));
        QCOMPARE(lane.contradictions.size(), 2);
        for (const Contradiction &contradiction : lane.contradictions)
            QCOMPARE(contradiction.kind, Contradiction::Kind::Date);
        QCOMPARE(lane.contradictions.at(0).setAside.edge, wrong);
        QCOMPARE(lane.contradictions.at(0).sources.size(), 3);
        QCOMPARE(lane.contradictions.at(1).setAside.edge, earlier);
        QCOMPARE(lane.contradictions.at(1).events, QList<qint64>({p, q, r}));
        // During and at the same time, the times must meet.
        Told apart;
        const qint64 one = apart.event("One"), two = apart.event("Two");
        apart.when(one, year(1900));
        apart.when(two, year(1901));
        apart.relate(one, "same time as", two);
        QCOMPARE(Chronology::chronicle(apart.facts).lanes.first().contradictions.size(), 1);
    }

    void oneTimePerEventByTheRule() {
        Told told;
        const qint64 siege = told.event("Siege");
        told.when(siege, year(1940), {1, 0, 0});
        const qint64 refined = told.when(siege, R"({"calendar": "gregorian", "at": "1940-05-10"})", {1, 5, 0});
        const qint64 vaguer = told.when(siege, year(1941), {2, 9, 0});
        // Within a book the last revealed; across books the most precise.
        QCOMPARE(Chronology::chosen(told.facts.times.value(siege))->fact, refined);
        auto chronicle = Chronology::chronicle(told.facts);
        QCOMPARE(chronicle.lanes.first().contradictions.size(), 1);
        QCOMPARE(chronicle.lanes.first().contradictions.first().kind, Contradiction::Kind::Times);
        QCOMPARE(chronicle.lanes.first().contradictions.first().setAside.fact, vaguer);
        // At the same precision, the first book's; and what was put in by hand above all.
        Told same;
        const qint64 flood = same.event("Flood");
        const qint64 firstBook = same.when(flood, year(1700), {3, 0, 0});
        same.when(flood, year(1701), {4, 0, 0});
        QCOMPARE(Chronology::chosen(same.facts.times.value(flood))->fact, firstBook);
        const qint64 byHand = same.when(flood, year(1702), {0, 0, 0});
        QCOMPARE(Chronology::chosen(same.facts.times.value(flood))->fact, byHand);
    }

    void timelinesShareAnAxisByCalendarOrByWhatLinksThem() {
        Told told;
        const qint64 one = told.timeline("One"), two = told.timeline("Two"), dream = told.timeline("Dream"),
                     waking = told.timeline("Waking");
        const qint64 a = told.event("A"), b = told.event("B"), c = told.event("C");
        told.when(a, year(1940));
        told.when(b, year(1950));
        told.when(c, year(1945));
        told.on(a, one);
        told.on(b, one);
        told.on(c, two);
        told.on(a, two); // on both
        const qint64 d = told.event("D"), e = told.event("E"), f = told.event("F");
        told.when(d, R"({"calendar": "the Dreaming", "at": "3"})");
        told.when(e, R"({"calendar": "the Dreaming", "at": "5"})");
        told.relate(f, "after", e);
        told.on(d, dream);
        told.on(e, dream);
        told.on(f, waking);
        const auto chronicle = Chronology::chronicle(told.facts);
        QCOMPARE(chronicle.lanes.size(), 4);
        QCOMPARE(order(laneOf(chronicle, "One")), QString("A < B"));
        QCOMPARE(order(laneOf(chronicle, "Two")), QString("A < C"));
        QCOMPARE(order(laneOf(chronicle, "Waking")), QString("F"));
        QCOMPARE(chronicle.axes.size(), 2);
        QCOMPARE(chronicle.axes.at(0).lanes.size() + chronicle.axes.at(1).lanes.size(), 4);
        for (const Chronology::Axis &axis : chronicle.axes) {
            const QString names = (axis.lanes | std::views::transform([&](int at) { return chronicle.lanes.at(at).timeline.name; })
                                   | std::ranges::to<QStringList>())
                                      .join(",");
            if (names.contains("One")) {
                QCOMPARE(names, QString("One,Two"));
                QCOMPARE(order(axis.order), QString("A < C < B"));
                QCOMPARE(axis.order.calendar, EventTime::gregorianName());
            } else {
                QCOMPARE(names, QString("Dream,Waking"));
                QCOMPARE(order(axis.order), QString("D < E < F"));
            }
        }
        const QJsonObject json = chronicle.toJson();
        QCOMPARE(json.value("axes").toArray().size(), 2);
        QCOMPARE(json.value("lanes").toArray().first().toObject().value("items").toArray().size(), 2);
    }

    void whatNothingPlacesIsListedApart() {
        Told told;
        const qint64 line = told.timeline("Line");
        const qint64 placed = told.event("Placed", {1, 0, 0});
        const qint64 words = told.event("Long ago", {1, 2, 0});
        const qint64 nothing = told.event("Nothing", {1, 1, 0});
        told.when(placed, year(1900));
        told.when(words, R"({"said": "long ago"})");
        for (const qint64 event : {placed, words, nothing})
            told.on(event, line);
        const auto chronicle = Chronology::chronicle(told.facts);
        QCOMPARE(order(laneOf(chronicle, "Line")), QString("Placed"));
        QCOMPARE(chronicle.unplaced.size(), 2);
        QCOMPARE(chronicle.unplaced.at(0).event.id, nothing);
        QCOMPARE(chronicle.unplaced.at(1).time->time.said, QString("long ago"));
        QCOMPARE(chronicle.unplaced.at(1).rank, -1);
    }

    void aSharedTimelineKeepsToItsTenant() {
        Told told;
        const qint64 history = told.timeline("History");
        const qint64 study = told.event("Study", {10, 0, 0}), leisure = told.event("Leisure", {20, 0, 0});
        const qint64 byHand = told.event("By hand", {0, 0, 0});
        for (const qint64 event : {study, leisure, byHand}) {
            told.when(event, year(1900 + event));
            told.on(event, history);
        }
        told.relate(study, "before", leisure);
        // Of a tenant's books: book 10 in one, book 20 in the other.
        const auto ofSchool = Chronology::chronicle(Chronology::within(told.facts, {10}));
        QCOMPARE(order(laneOf(ofSchool, "History")), QString("Study < By hand"));
        QVERIFY(ofSchool.toJson().value("lanes").toArray().size() == 1);
        const auto ofHome = Chronology::chronicle(Chronology::within(told.facts, {20}));
        QCOMPARE(order(laneOf(ofHome, "History")), QString("Leisure < By hand"));
        // A tenant with both, all of it.
        QCOMPARE(order(laneOf(Chronology::chronicle(Chronology::within(told.facts, {10, 20})), "History")),
                 QString("Study < Leisure < By hand"));
    }

    // An order, a time or an event revealed later in the book shows only once
    // the reader is there.
    void anOrderRevealedLaterDoesNotShowEarly() {
        Reading reading;
        Graph graph(reading.library);
        const qint64 book = reading.book;
        const auto at = [book](int section) { return GraphMoment{{book, section, 0.5}, QString("in chapter %1").arg(section)}; };
        const qint64 siege = graph.upsert({}, "event", "The siege", {}, {}, GraphPlace{book, 0, 0.2});
        const qint64 flood = graph.upsert({}, "event", "The flood", {}, {}, GraphPlace{book, 1, 0.5});
        const qint64 fall = graph.upsert({}, "event", "The fall", {}, {}, GraphPlace{book, 8, 0.5});
        const qint64 age = Chronology::timeline(graph, Chronology::Whose::World, "The Third Age", at(0));
        QVERIFY(Chronology::setWhen(graph, siege, timeOf(R"({"calendar": "Third Age", "at": "3019"})"), at(0)));
        QVERIFY(Chronology::setWhen(graph, flood, timeOf(R"({"calendar": "Third Age", "at": "3020"})"), at(1)));
        // A flashback: the flood came first after all.
        QVERIFY(Chronology::setWhen(graph, flood, timeOf(R"({"calendar": "Third Age", "at": "3018"})"), at(6)));
        QVERIFY(Chronology::order(graph, fall, "after", siege, at(8)));
        for (const qint64 event : {siege, flood, fall})
            QVERIFY(Chronology::placeOn(graph, event, age, at(0)));

        // A book not begun: its reader at its start, before the siege at 0.2.
        reading.library.setProgress(book, {});
        QVERIFY(reading.chronicle().lanes.isEmpty());
        reading.readTo(2);
        const auto early = reading.chronicle();
        QCOMPARE(order(laneOf(early, "The Third Age")), QString("The siege < The flood"));
        QVERIFY(!QString::fromUtf8(written(early)).contains("The fall"));
        QVERIFY(!QString::fromUtf8(written(early)).contains("3018"));
        reading.readTo(7);
        QCOMPARE(order(laneOf(reading.chronicle(), "The Third Age")), QString("The flood < The siege"));
        reading.readTo(9);
        QCOMPARE(order(laneOf(reading.chronicle(), "The Third Age")), QString("The flood < The siege < The fall"));
        // One tenant where the library isn't split.
        QCOMPARE(Chronology::tenants(reading.library).size(), 1);
    }

    void theSameFactsInAnyOrderGiveTheSameChronicle() {
        const Told told = many(300, 43);
        const QByteArray once = written(Chronology::chronicle(told.facts));
        QRandomGenerator random(7);
        for (int round = 0; round < 5; ++round)
            QCOMPARE(written(Chronology::chronicle(shuffled(told.facts, random))), once);
        QVERIFY(once.contains("\"contradictions\":[{"));
    }

    void thousandsOfEventsOrderQuickly() {
        const Told told = many(3000, 2026);
        QElapsedTimer clock;
        clock.start();
        const auto chronicle = Chronology::chronicle(told.facts);
        const qint64 took = clock.elapsed();
        qInfo("3000 events, %lld relations: %lld ms", static_cast<long long>(told.facts.edges.size() - 3000), static_cast<long long>(took));
        QCOMPARE(chronicle.lanes.size(), 1);
        QVERIFY(chronicle.lanes.first().items.size() + chronicle.unplaced.size() == 3000);
        QVERIFY2(took < 100, qPrintable(QString("%1 ms").arg(took)));
        // Every relation kept is kept by the order.
        QHash<qint64, int> rank;
        for (const Chronology::Item &item : chronicle.lanes.first().items)
            rank.insert(item.event.id, item.rank);
        QSet<qint64> setAside;
        for (const Contradiction &contradiction : chronicle.lanes.first().contradictions)
            setAside.insert(contradiction.setAside.edge);
        for (const GraphEdge &edge : told.facts.edges)
            if (edge.kind != "on" && !setAside.contains(edge.id))
                QVERIFY(edge.kind == "before" ? rank.value(edge.source) < rank.value(edge.target)
                                              : rank.value(edge.target) < rank.value(edge.source));
    }
};

QTEST_MAIN(ChronicleTests)
#include "chronicle_tests.moc"
