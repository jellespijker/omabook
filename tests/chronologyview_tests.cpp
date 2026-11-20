// The chronology as the window lays it out and draws it (#44): bands of
// lanes on shared axes, each event where its time puts it (or between the
// events with one, by its order), spans as spans, an event during another
// under it; the ruler of a calendar; the filters; never ahead of the
// reader; and thousands of events drawn within a frame.

#include <QElapsedTimer>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <ranges>

#include "calendars.h"
#include "chronicle.h"
#include "chronology.h"
#include "chronologycanvas.h"
#include "chronologychart.h"
#include "chronologymodel.h"
#include "eventtime.h"
#include "fixtures.h"
#include "graph.h"
#include "library.h"

using namespace Chronology;

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
    Facts facts;
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
        node.kind = timelineKind();
        node.name = name;
        facts.timelines.append(node);
        return id;
    }
    void when(qint64 event, const QString &json) {
        facts.times[event].append({.time = timeOf(json), .revealed = {{1, 0, 0}, {}}, .fact = next++});
    }
    void relate(qint64 event, const QString &relation, qint64 other) {
        facts.edges.append(
            {.id = next++, .source = event, .target = other, .kind = relation, .note = {}, .learned = {1, 0, 0}, .anchor = {}});
    }
    void on(qint64 event, qint64 timeline) { relate(event, onRelation(), timeline); }

    [[nodiscard]] Chart::Chart chart() const { return Chart::chart({{.name = {}, .chronicle = chronicle(facts)}}); }
};

[[nodiscard]] const Chart::Mark &markOf(const Chart::Chart &chart, qint64 event) {
    return chart.marks.at(chart.marksOf.value(event).first());
}

// A library of a made-up war: a book of it being read, its events on real
// history, some dated, some only in order, one during another, and one of
// which nothing is known but that it happened.
struct War {
    QTemporaryDir dir;
    Library library{dir.filePath("library")};
    qint64 book = 0, other = 0;
    qint64 invasion = 0, siege = 0, sortie = 0, flood = 0, rumour = 0, later = 0, elsewhere = 0, mara = 0;

    bool make() {
        if (!library.open())
            return false;
        BookMeta meta;
        meta.title = "The Long Siege";
        meta.series = "The War";
        book = library.addEntry(meta, false).id;
        meta.title = "The Harbour";
        meta.series.clear();
        other = library.addEntry(meta, false).id;
        library.setReading(book, "reading", 0, {});
        library.setProgress(book, {2, 0.5, 40, 0});
        library.setReading(other, "read", 0, {});
        Graph graph(library);
        const auto at = [this](int section, double fraction) { return GraphPlace{book, section, fraction}; };
        const qint64 history = Chronology::timeline(graph, Whose::History, "Real history", GraphMoment{at(0, 0.1), {}});
        const auto made = [&](const QString &name, const GraphPlace &place) {
            const qint64 id = graph.upsert({}, "event", name, {}, {}, place, name);
            return Chronology::placeOn(graph, id, history, GraphMoment{place, {}}) ? id : 0;
        };
        invasion = made("The invasion", at(0, 0.2));
        siege = made("The siege", at(1, 0.1));
        sortie = made("The sortie", at(1, 0.5));
        flood = made("The flood", at(2, 0.2));
        later = made("What happens after the page", at(3, 0.5));
        rumour = graph.upsert({}, "event", "A rumour of peace", {}, {}, at(2, 0.3), "rumour");
        elsewhere = graph.upsert({}, "event", "The harbour burns", {}, {}, GraphPlace{other, 0, 0.5}, "burns");
        mara = graph.upsert({}, "character", "Mara", {}, {}, at(0, 0.1), "Mara");
        graph.link(sortie, mara, "led by", {}, at(1, 0.5));
        return Chronology::setWhen(graph, invasion, timeOf(year(1939)), GraphMoment{at(0, 0.2), "in 1939"})
               && Chronology::setWhen(graph, siege, timeOf(R"({"calendar": "gregorian", "from": "1941-09", "to": "1944-01"})"),
                                      GraphMoment{at(1, 0.1), "the siege"})
               && Chronology::order(graph, sortie, "during", siege, GraphMoment{at(1, 0.5), {}})
               && Chronology::setWhen(graph, flood, timeOf(year(1945)), GraphMoment{at(2, 0.2), "1945"})
               && Chronology::setWhen(graph, later, timeOf(year(1946)), GraphMoment{at(3, 0.5), "1946"})
               && Chronology::setWhen(graph, elsewhere, timeOf(year(1940)), GraphMoment{{other, 0, 0.5}, "1940"});
    }
};

} // namespace

class ChronologyViewTests : public QObject {
    Q_OBJECT

private slots:
    // --- The ruler ---

    void aMinuteOfTheAxisIsReadBackAsItsMoment() {
        // The real calendar, before and after the year 0.
        const Calendars calendars({{.time = timeOf(year(1944)), .revealed = {}, .fact = 1}});
        const Calendars::Scale real = calendars.scale(EventTime::gregorianName());
        for (const char *text : {"1944-06-06T06:30", "2000-02-29T23:59", "0-01-01T00:00", "-43-03-15T11:00", "1-12-31T12:00"}) {
            const auto point = EventTime::Point::parse(QString::fromLatin1(text), true);
            QVERIFY(point);
            EventTime time;
            time.on = {.calendar = EventTime::gregorianName(), .at = *point, .from = {}, .to = {}};
            const auto extent = real.extent(time);
            QVERIFY(extent && extent->from);
            QCOMPARE(real.point(*extent->from).text(), QString::fromLatin1(text));
        }
        // A story's own, as long as its times show it is: thirteen months of forty days.
        const Calendars own({{.time = timeOf(R"({"calendar": "Long Count", "at": "7-13-40"})"), .revealed = {}, .fact = 1}});
        const Calendars::Scale count = own.scale("Long Count");
        EventTime time = timeOf(R"({"calendar": "Long Count", "at": "7-13-40"})");
        QCOMPARE(count.point(*count.extent(time)->from).text(), QString("7-13-40T00:00"));
        time = timeOf(R"({"calendar": "Long Count", "at": "-2-01-01"})");
        QCOMPARE(count.point(*count.extent(time)->from).text(), QString("-2-01-01T00:00"));
    }

    void aRulerGoesByTheRoundestStepThatFits() {
        const Calendars calendars({{.time = timeOf(year(1944)), .revealed = {}, .fact = 1}});
        const Calendars::Scale real = calendars.scale(EventTime::gregorianName());
        const auto minuteOf = [&real](const QString &json) { return *real.extent(timeOf(json))->from; };
        const auto texts = [](const QList<Calendars::Scale::Tick> &ticks) {
            return ticks | std::views::transform([](const Calendars::Scale::Tick &tick) { return tick.point.text(); })
                   | std::ranges::to<QStringList>();
        };
        // Centuries across six hundred years, years across a decade, months
        // across a year, hours across a day, minutes across an hour.
        QCOMPARE(texts(real.ticks(minuteOf(year(1400)), minuteOf(year(2000)), 8)),
                 QStringList({"1400", "1500", "1600", "1700", "1800", "1900", "2000"}));
        QCOMPARE(texts(real.ticks(minuteOf(year(1939)), minuteOf(year(1946)), 10)),
                 QStringList({"1939", "1940", "1941", "1942", "1943", "1944", "1945", "1946"}));
        QCOMPARE(texts(real.ticks(minuteOf(year(1944)), minuteOf(year(1945)), 5)),
                 QStringList({"1944", "1944-04", "1944-07", "1944-10", "1945"}));
        const qint64 dday = minuteOf(R"({"calendar": "gregorian", "at": "1944-06-06"})");
        QCOMPARE(texts(real.ticks(dday, dday + 24 * 60, 5)),
                 QStringList({"1944-06-06", "1944-06-06T06", "1944-06-06T12", "1944-06-06T18", "1944-06-07"}));
        // Each as far as it needs: the hour is the hour, not its first minute.
        QCOMPARE(texts(real.ticks(dday + 6 * 60, dday + 7 * 60, 4)),
                 QStringList({"1944-06-06T06", "1944-06-06T06:15", "1944-06-06T06:30", "1944-06-06T06:45", "1944-06-06T07"}));
        // Before the common era too, and never more than asked for (twice, at a stretch's edges).
        const QList<Calendars::Scale::Tick> ancient = real.ticks(minuteOf(year(-3000)), minuteOf(year(-500)), 6);
        QVERIFY(!ancient.isEmpty() && ancient.size() <= 12);
        QCOMPARE(ancient.first().point.text(), QString("-3000"));
        QVERIFY(std::ranges::is_sorted(ancient, {}, &Calendars::Scale::Tick::minute));
    }

    // --- The layout ---

    void eventsAreWhereTheirTimesPutThemAndTheRestBetween() {
        Told told;
        const qint64 history = told.timeline("Real history");
        const qint64 a = told.event("A"), b = told.event("B"), c = told.event("C"), d = told.event("D");
        told.when(a, year(1900));
        told.when(c, year(2000));
        told.when(d, R"({"calendar": "gregorian", "from": "1950", "to": "1960"})");
        // B only comes after A and before C.
        told.relate(b, "after", a);
        told.relate(b, "before", c);
        for (const qint64 event : {a, b, c, d})
            told.on(event, history);
        const Chart::Chart chart = told.chart();
        QCOMPARE(chart.bands.size(), 1);
        QVERIFY(chart.bands.first().dated());
        const Chart::Mark &first = markOf(chart, a), &between = markOf(chart, b), &last = markOf(chart, c), &span = markOf(chart, d);
        // Dated ones by their time, a moment in the middle of its year.
        QVERIFY(first.dated && last.dated && !between.dated);
        const Chart::Band &band = chart.bands.first();
        const auto yearAt = [&band](double along) { return band.scale->point(static_cast<qint64>(band.value(along))).year; };
        QCOMPARE(yearAt(first.from), 1900);
        QCOMPARE(yearAt(last.from), 2000);
        // The one of no time between its neighbours, and a span as long as it is.
        QVERIFY(first.from < between.from && between.from < last.from);
        QVERIFY(span.span && !first.span);
        QCOMPARE(yearAt(span.from), 1950);
        QCOMPARE(yearAt(span.to - 1e-9), 1960);
        // With a margin either side, inside the band.
        QVERIFY(first.from >= Chart::margin - 1e-9 && last.to <= 1 - Chart::margin + 1e-9);
    }

    void lanesThatLineUpShareABandAndAnEventDuringAnotherIsUnderIt() {
        Told told;
        const qint64 history = told.timeline("Real history"), city = told.timeline("The city"), dream = told.timeline("The dream");
        const qint64 war = told.event("The war"), battle = told.event("The battle"), fire = told.event("The fire");
        const qint64 wake = told.event("Waking"), sleep = told.event("Sleeping"), lost = told.event("Something, sometime");
        told.when(war, R"({"calendar": "gregorian", "from": "1939", "to": "1945"})");
        told.relate(battle, "during", war);
        told.when(fire, year(1941));
        told.on(war, history);
        told.on(battle, history);
        told.on(fire, city);
        // A timeline of its own, ordered and undated, which lines up with nothing.
        told.relate(sleep, "before", wake);
        told.on(sleep, dream);
        told.on(wake, dream);
        const Chart::Chart chart = told.chart();
        QCOMPARE(chart.bands.size(), 2);
        const auto bandOf = [&chart](qint64 event) { return chart.lanes.at(markOf(chart, event).lane).band; };
        // Real history and the city's are on one calendar: one band, two lanes.
        QCOMPARE(bandOf(war), bandOf(fire));
        QVERIFY(markOf(chart, war).lane != markOf(chart, fire).lane);
        QCOMPARE(chart.bands.at(bandOf(war)).lanes.size(), 2);
        // The dream's is by order alone.
        QVERIFY(!chart.bands.at(bandOf(wake)).dated());
        QVERIFY(markOf(chart, sleep).from < markOf(chart, wake).from);
        // The battle, during the war, is a row under it.
        QCOMPARE(markOf(chart, battle).within, war);
        QCOMPARE(markOf(chart, battle).row, markOf(chart, war).row + 1);
        QVERIFY(chart.lanes.at(markOf(chart, war).lane).rows >= 2);
        // What has no time or order is listed apart, not on a lane.
        QVERIFY(!chart.marksOf.contains(lost));
        QCOMPARE(chart.unplaced.size(), 1);
        QCOMPARE(chart.unplaced.first().event.id, lost);
    }

    void overlappingSpansGoOnRowsOfTheirOwn() {
        Told told;
        const qint64 history = told.timeline("Real history");
        QList<qint64> spans;
        for (const char *range :
             {R"("from": "1900", "to": "1950")", R"("from": "1920", "to": "1930")", R"("from": "1960", "to": "1970")"}) {
            spans << told.event(QString::fromLatin1(range));
            told.when(spans.last(), QString(R"({"calendar": "gregorian", %1})").arg(QString::fromLatin1(range)));
            told.on(spans.last(), history);
        }
        const Chart::Chart chart = told.chart();
        QVERIFY(markOf(chart, spans.at(0)).row != markOf(chart, spans.at(1)).row);
        // One after both ends goes back to the first row.
        QCOMPARE(markOf(chart, spans.at(2)).row, 0);
    }

    void aContradictionMarksItsEvents() {
        Told told;
        const qint64 history = told.timeline("Real history");
        const qint64 a = told.event("A"), b = told.event("B"), c = told.event("C");
        told.relate(a, "before", b);
        told.relate(b, "before", a);
        for (const qint64 event : {a, b, c})
            told.on(event, history);
        told.relate(c, "after", b);
        const Chart::Chart chart = told.chart();
        QVERIFY(markOf(chart, a).contradicted && markOf(chart, b).contradicted);
        QVERIFY(!markOf(chart, c).contradicted);
        QCOMPARE(Chart::contradictionsOf(chart, a).size(), 1);
        QCOMPARE(Chart::contradictionsOf(chart, a).first().kind, Contradiction::Kind::Cycle);
        QVERIFY(Chart::contradictionsOf(chart, c).isEmpty());
    }

    // --- The browser ---

    void aBooksChronologyIsAsFarAsItsReaderRead() {
        War war;
        QVERIFY(war.make());
        ChronologyBrowser browser;
        browser.setLibrary(&war.library);
        browser.showBook(war.book);
        QCOMPARE(browser.showing(), QString("book"));
        QCOMPARE(browser.title(), QString("The Long Siege"));
        const Chart::Chart &chart = browser.chart();
        // What happens after the page is not there at all; what was only met is listed apart.
        QVERIFY(!chart.items.contains(war.later));
        QVERIFY(chart.marksOf.contains(war.invasion) && chart.marksOf.contains(war.siege) && chart.marksOf.contains(war.sortie));
        QCOMPARE(browser.unplaced().size(), 1);
        QCOMPARE(browser.unplaced().first().toMap().value("id").toLongLong(), war.rumour);
        // The other book's event is of the whole library, but the book is the filter.
        QVERIFY(chart.marksOf.contains(war.elsewhere));
        QCOMPARE(browser.filters().size(), 1);
        QCOMPARE(browser.shownCount(), 4);
        QVERIFY(!browser.passes().at(chart.marksOf.value(war.elsewhere).first()));
        // You are here: the event met last, as far as it was read.
        QCOMPARE(browser.here(), war.flood);
        // Read on, and what happens after is there.
        war.library.setProgress(war.book, {3, 0.9, 80, 0});
        browser.refresh();
        QVERIFY(browser.chart().marksOf.contains(war.later));
        QCOMPARE(browser.here(), war.later);
    }

    void theFiltersNarrowItAndComeOffAgain() {
        War war;
        QVERIFY(war.make());
        ChronologyBrowser browser;
        browser.setLibrary(&war.library);
        browser.showScope();
        QCOMPARE(browser.filters().size(), 0);
        QCOMPARE(browser.shownCount(), 5);
        QSignalSpy changed(&browser, &ChronologyBrowser::changed);
        // Who an event involves.
        browser.setFilter("involved", war.mara, true);
        QCOMPARE(browser.shownCount(), 1);
        QCOMPARE(changed.size(), 1);
        browser.setFilter("involved", war.mara, false);
        // A book, and a series.
        browser.setFilter("book", war.other, true);
        QCOMPARE(browser.shownCount(), 1);
        browser.setFilter("book", war.other, false);
        browser.setFilter("series", "series:The War", true);
        QCOMPARE(browser.shownCount(), 4);
        // A stretch of time: the siege runs into it, and the sortie during
        // it goes between it and the flood; the invasion is before it.
        QVERIFY(browser.setRange("1943", "1946"));
        QCOMPARE(browser.shownCount(), 3);
        QVERIFY(!browser.setRange("the war", ""));
        QCOMPARE(browser.filters().size(), 2);
        // Every one off at once.
        browser.clearFilters();
        QCOMPARE(browser.shownCount(), 5);
        // What the filter's list offers: a lane, the books, Mara.
        const QVariantList choices = browser.choices();
        const auto offers = [&choices](const QString &act) {
            return std::ranges::count_if(choices, [&act](const QVariant &choice) { return choice.toMap().value("act") == act; });
        };
        // Real history, and the harbour's own order (its event is on no timeline).
        QCOMPARE(offers("timeline"), 2);
        QCOMPARE(offers("book"), 2);
        QCOMPARE(offers("involved"), 1);
        // Only what was met in a book, and the list of what has no time hidden.
        browser.setShowUnplaced(false);
        QVERIFY(browser.unplaced().isEmpty());
        QVERIFY(browser.find("sortie").size() == 1);
    }

    void theCardSaysWhenAndWhereItWasRevealed() {
        War war;
        QVERIFY(war.make());
        ChronologyBrowser browser;
        browser.setLibrary(&war.library);
        browser.showBook(war.book);
        browser.select(war.siege);
        const QVariantMap card = browser.details();
        QCOMPARE(card.value("name").toString(), QString("The siege"));
        QVERIFY2(card.value("when").toString().contains("1941") && card.value("when").toString().contains("1944"),
                 qPrintable(card.value("when").toString()));
        QCOMPARE(card.value("timelines").toStringList(), QStringList({"Real history"}));
        // Back to the page: where its time was revealed, with the words there.
        const QVariantMap page = card.value("page").toMap();
        QCOMPARE(page.value("bookId").toLongLong(), war.book);
        QCOMPARE(page.value("section").toInt(), 1);
        QCOMPARE(page.value("anchor").toString(), QString("the siege"));
        // And during it, what it involves.
        browser.select(war.sortie);
        QCOMPARE(browser.details().value("during").toString(), QString("The siege"));
        QCOMPARE(browser.details().value("involved").toList().size(), 1);
        // Stepping along the lane.
        browser.select(war.invasion);
        browser.step("right");
        QCOMPARE(browser.selected(), war.siege);
        browser.step("left");
        QCOMPARE(browser.selected(), war.invasion);
    }

    void aWholeGraphIsShownASideAtATime() {
        War war;
        QVERIFY(war.make());
        // The two books in two top collections, kept apart.
        const auto study = war.library.addCollection("Study"), leisure = war.library.addCollection("Leisure");
        QVERIFY(study && leisure);
        QVERIFY(war.library.putInCollection(*study, war.book, true) && war.library.putInCollection(*leisure, war.other, true));
        ChronologyBrowser browser;
        browser.setLibrary(&war.library);
        browser.showScope();
        const Chart::Chart &chart = browser.chart();
        // Two parts, each its own bands: the harbour's year is never on the war's axis.
        QSet<int> parts;
        for (const Chart::Band &band : chart.bands)
            parts.insert(band.part);
        QVERIFY2(parts.size() == 2, qPrintable(QString::number(parts.size())));
        const auto partOf = [&chart](qint64 event) { return chart.bands.at(chart.lanes.at(markOf(chart, event).lane).band).part; };
        QVERIFY(partOf(war.elsewhere) != partOf(war.invasion));
        QVERIFY(browser.bandName(chart.lanes.at(markOf(chart, war.elsewhere).lane).band).contains("Leisure"));
        // Opened from the book, only its side is read.
        browser.showBook(war.book);
        QVERIFY(!browser.chart().marksOf.contains(war.elsewhere));
    }

    // --- Drawing ---

    void thousandsOfEventsAreDrawnWithinAFrame() {
        // Three thousand events of a book read to its end, on twelve
        // timelines over two thousand years: a third of them moments, a
        // third spans, a third only in order.
        QTemporaryDir dir;
        Library library(dir.filePath("library"));
        QVERIFY(library.open());
        BookMeta meta;
        meta.title = "A Very Long Chronicle";
        const qint64 book = library.addEntry(meta, false).id;
        library.setReading(book, "read", 0, {});
        {
            const Library::Batch batch(library);
            Graph graph(library);
            QList<qint64> timelines;
            for (int at = 0; at < 12; ++at)
                timelines << Chronology::timeline(graph, Whose::World, QString("Timeline %1").arg(at),
                                                  GraphMoment{GraphPlace{book, 0, 0}, {}});
            qint64 before = 0;
            for (int at = 0; at < 3000; ++at) {
                const GraphPlace place{book, at / 100, (at % 100) / 100.0};
                const qint64 event = graph.upsert({}, "event", QString("Event number %1").arg(at), {}, {}, place);
                const auto timed =
                    at % 3 == 0 ? Chronology::setWhen(graph, event, timeOf(year(at * 2 / 3)), GraphMoment{place, {}})
                    : at % 3 == 1
                        ? Chronology::setWhen(
                              graph, event,
                              timeOf(QString(R"({"calendar": "gregorian", "from": "%1", "to": "%2"})").arg(at * 2 / 3).arg(at * 2 / 3 + 5)),
                              GraphMoment{place, {}})
                        : Chronology::order(graph, event, "after", before, GraphMoment{place, {}});
                QVERIFY2(timed, qPrintable(Fixtures::whyNot(timed)));
                QVERIFY(Chronology::placeOn(graph, event, timelines.at(at % 12), GraphMoment{place, {}}));
                before = event;
            }
        }
        ChronologyBrowser browser;
        browser.setLibrary(&library);
        QElapsedTimer clock;
        clock.start();
        browser.showScope();
        const qint64 read = clock.elapsed();
        const Chart::Chart &chart = browser.chart();
        QCOMPARE(chart.marksOf.size(), 3000);

        // In a window, as the scene graph draws it.
        QQuickWindow window;
        window.resize(1180, 760);
        ChronologyCanvas canvas(window.contentItem());
        canvas.setSize(QSizeF(1180, 760));
        canvas.setColors({{"red", QColor("#f7768e")}, {"muted", QColor("#5c5c66")}, {"yellow", QColor("#e0af68")}});
        canvas.setHereText("You are here");
        canvas.setBrowser(&browser);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto frame = [&] {
            QSignalSpy drawn(&window, &QQuickWindow::frameSwapped);
            canvas.update();
            return drawn.wait(2000);
        };
        QVERIFY(frame());
        const double whole = canvas.frameTime();
        const int all = canvas.drawn();
        // Zoomed in a hundred times on the middle, most of it out of view.
        canvas.zoomBy(100);
        QVERIFY(frame());
        const double near = canvas.frameTime();
        canvas.panBy(-300, -200);
        QVERIFY(frame());
        const double panned = canvas.frameTime();
        qInfo() << "3000 events: read and laid out in" << read << "ms; a frame of" << all << "marks in" << whole << "ms, zoomed in" << near
                << "ms, panned" << panned << "ms";
        // Well within a frame (16 ms), on a machine several times slower than this one.
        QVERIFY(all > 300);
        QVERIFY2(whole < 8, qPrintable(QString::number(whole)));
        QVERIFY2(near < 8 && panned < 8, qPrintable(QString("%1 %2").arg(near).arg(panned)));
        // A point on a mark finds its event.
        canvas.fit();
        const qint64 some = chart.marks.at(chart.marks.size() / 2).event;
        browser.select(some);
        QVERIFY(frame());
        const QPointF at = canvas.pointOf(some);
        QCOMPARE(canvas.eventAt(at.x(), at.y()), some);
        QCOMPARE(canvas.eventAt(-500, -500), qint64(0));
    }
};

QTEST_MAIN(ChronologyViewTests)
#include "chronologyview_tests.moc"
