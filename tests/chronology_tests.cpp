// The chronology's model (#41): when an event happens (a "when" fact, an
// EventTime), its order against other events, and the timelines it is on.
// They are ordinary facts and relations of the graph, each with the moment it
// was revealed, so a reader is told no more of them than they have read, in
// their own graph and in a graph shared with them (#5); and they travel to
// another computer through the change log like the rest of the graph.

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

#include "chronology.h"
#include "eventtime.h"
#include "fixtures.h"
#include "graph.h"
#include "graphcontext.h"
#include "library.h"
#include "person.h"
#include "publishedspace.h"
#include "remote.h"
#include "subscriptions.h"
#include "sync.h"

namespace {

[[nodiscard]] EventTime timeOf(const char *json) {
    const auto time = EventTime::parse(QString::fromUtf8(json));
    if (!time)
        qFatal("%s", qPrintable(time.error()));
    return *time;
}

[[nodiscard]] bool refused(const char *json) { return !EventTime::parse(QString::fromUtf8(json)).has_value(); }

[[nodiscard]] QStringList relations(const Chronology::Known &known) {
    QStringList list;
    for (const Chronology::Ordered &other : known.order)
        list << other.relation + " " + other.other.name;
    list.sort();
    return list;
}

[[nodiscard]] QStringList timelines(const Chronology::Known &known) {
    QStringList list;
    for (const Chronology::Placed &placed : known.timelines)
        list << placed.timeline.name;
    list.sort();
    return list;
}

[[nodiscard]] QStringList times(const Chronology::Known &known) {
    QStringList list;
    for (const Chronology::Dated &dated : known.times)
        list << dated.time.written();
    return list;
}

[[nodiscard]] bool tenChapters(const QString &path) {
    QList<Fixtures::Chapter> chapters;
    for (int at = 1; at <= 10; ++at)
        chapters << Fixtures::Chapter{QString("Chapter %1").arg(at), QString("<p>The siege went on, the %1th day.</p>").arg(at)};
    return Fixtures::chapteredEpub(path, "The Long Siege", chapters);
}

// A story of ten chapters and what it tells of when things happen: the siege
// is met first, dated to a year and later to its day; the flood is met next,
// and only a flashback near the end tells that the siege came before it; the
// fall is met last of all. The Third Age is the world's timeline from the
// start; that the siege is real history too is told halfway.
struct Story {
    explicit Story(Library &library, qint64 book) : graph(library), book(book) {
        const auto at = [book](int section, const QString &anchor) { return GraphMoment{{book, section, 0.5}, anchor}; };
        siege = graph.upsert({}, "event", "The siege", {}, {}, GraphPlace{book, 0, 0.2}, "the siege");
        flood = graph.upsert({}, "event", "The flood", {}, {}, GraphPlace{book, 1, 0.5}, "the flood");
        fall = graph.upsert({}, "event", "The fall", {}, {}, GraphPlace{book, 8, 0.5}, "the fall");
        QVERIFY(siege > 0 && flood > 0 && fall > 0);
        QVERIFY(Chronology::setWhen(graph, siege, timeOf(R"({"calendar": "Third Age", "at": "3019"})"), at(0, "in 3019")));
        QVERIFY(Chronology::setWhen(graph, siege, timeOf(R"({"calendar": "Third Age", "at": "3019-03-15"})"), at(7, "on the ides")));
        QVERIFY(Chronology::order(graph, siege, "before", flood, at(8, "long before the flood")));
        QVERIFY(Chronology::order(graph, fall, "after", siege, at(8, "after the siege")));
        world = Chronology::timeline(graph, Chronology::Whose::World, "The Third Age", at(0, "the Third Age"));
        history = Chronology::timeline(graph, Chronology::Whose::History, "History", at(4, "as history tells"));
        QVERIFY(world > 0 && history > 0);
        QVERIFY(Chronology::placeOn(graph, siege, world, at(0, "the Third Age")));
        QVERIFY(Chronology::placeOn(graph, siege, history, at(4, "as history tells")));
    }

    Graph graph;
    qint64 book = 0, siege = 0, flood = 0, fall = 0, world = 0, history = 0;
};

// A book of no file, read to a place.
struct Reading {
    Reading() : library(dir.filePath("Library")) {
        QVERIFY(library.open());
        BookMeta meta;
        meta.title = "The Long Siege";
        book = library.addEntry(meta, true).id;
        QVERIFY(book > 0);
    }
    void readTo(int section) { library.setProgress(book, {section, 0.9, section * 10.0}); }
    [[nodiscard]] std::optional<Chronology::Known> known(qint64 event) const {
        return Chronology::of(library, event, GraphContext::readerKnows(library));
    }

    QTemporaryDir dir;
    Library library;
    qint64 book = 0;
};

[[nodiscard]] SpaceSnapshot::Owner ada() {
    static const auto person = Person::generate();
    if (!person)
        qFatal("%s", qPrintable(person.error()));
    return {.name = "Ada Fixture", .person = person->id()};
}

[[nodiscard]] const GraphNode *named(const GraphLayers::Layer &layer, const QString &name) {
    const auto found = std::ranges::find(layer.nodes, name, &GraphNode::name);
    return found == layer.nodes.cend() ? nullptr : &*found;
}

} // namespace

class ChronologyTests : public QObject {
    Q_OBJECT

private slots:
    void aTimeIsAMomentOrASpanOnACalendarWithItsPrecision() {
        const EventTime day = timeOf(R"({"calendar": "Gregorian", "at": "1944-06-06", "said": " D-Day "})");
        QVERIFY(day.on.gregorian() && !day.span());
        QCOMPARE(day.precision(), EventTime::Precision::Day);
        QCOMPARE(day.written(), QString(R"({"at":"1944-06-06","calendar":"gregorian","said":"D-Day"})"));
        QCOMPARE(EventTime::kept(day.written()), day.written());
        QCOMPARE(timeOf(R"({"calendar": "gregorian", "at": "1944-06-06T06:30"})").precision(), EventTime::Precision::Minute);
        QCOMPARE(timeOf(R"({"calendar": "gregorian", "at": "-43-03-15T11"})").precision(), EventTime::Precision::Hour);
        QCOMPARE(timeOf(R"({"calendar": "gregorian", "at": "2024-02-29"})").precision(), EventTime::Precision::Day);
        const EventTime war = timeOf(R"({"calendar": "gregorian", "from": "1939-09-01", "to": "1945"})");
        QVERIFY(war.span());
        QCOMPARE(war.precision(), EventTime::Precision::Year);
        QVERIFY(timeOf(R"({"calendar": "gregorian", "from": "1945-03", "to": "1945"})").span()); // within its year
        QVERIFY(timeOf(R"({"calendar": "gregorian", "to": "1066-10-14"})").span());
        // A story's own calendar, tied to another where the book says so.
        const EventTime age = timeOf(R"({"calendar": "Third Age", "at": "3019-13-40", "approximate": true,
                                          "also": [{"calendar": "Shire Reckoning", "at": "1419"}]})");
        QVERIFY(!age.on.gregorian() && age.approximate);
        QCOMPARE(age.also.size(), 1);
        QCOMPARE(age.also.first().at->text(), QString("1419"));
        // Only words for it: a time the book gives no date for.
        QCOMPARE(timeOf(R"({"said": "long before the war"})").precision(), EventTime::Precision::None);

        for (const char *wrong :
             {R"({"calendar": "gregorian", "at": "1944-13-01"})", R"({"calendar": "gregorian", "at": "2023-02-29"})",
              R"({"calendar": "gregorian", "at": "1944-06-06T24"})", R"({"calendar": "gregorian", "at": "June 1944"})", R"({"at": "1944"})",
              R"({"calendar": "gregorian", "at": "1944", "from": "1943"})", R"({"calendar": "gregorian", "from": "1945", "to": "1939"})",
              R"({"calendar": "gregorian"})", R"({"calendar": "gregorian", "at": "1944", "when": "then"})",
              R"({"calendar": "x", "at": 1944})", R"({"calendar": "x", "at": "1", "also": [{"calendar": "x", "at": "2"}]})",
              R"({"said": "then", "also": [{"calendar": "y", "at": "2"}]})", "[]", "not json"})
            QVERIFY2(refused(wrong), wrong);
        QVERIFY(refused(QByteArray(R"({"said": ")" + QByteArray(200, 'a') + R"("})").constData()));
        QVERIFY(EventTime::kept(u"{\"at\": \"1944\"}").isEmpty());
    }

    void aTimeIsKeptOnlyOnAnEventAndOnlyWhenItIsOne() {
        const Reading reading;
        Graph graph(reading.library);
        const qint64 siege = graph.upsert({}, "event", "The siege");
        const qint64 mara = graph.upsert({}, "character", "Mara");
        const EventTime year = timeOf(R"({"calendar": "gregorian", "at": "1453"})");
        QVERIFY(!Chronology::setWhen(graph, mara, year));
        EventTime backwards = timeOf(R"({"calendar": "gregorian", "from": "1453", "to": "1454"})");
        std::swap(backwards.on.from, backwards.on.to);
        QVERIFY(!Chronology::setWhen(graph, siege, backwards));
        // Not by learn() either, and the same time said twice is one fact.
        QCOMPARE(graph.learn(siege, "when", R"({"calendar": "gregorian", "at": "1453-02-30"})"), 0);
        const auto kept = Chronology::setWhen(graph, siege, year);
        QVERIFY2(kept.has_value(), qPrintable(Fixtures::whyNot(kept)));
        QVERIFY(graph.learn(siege, "when", R"({ "at": "1453", "calendar": "Gregorian" })") > 0);
        QCOMPARE(graph.facts(siege).size(), 1);
        // A time is no summary and no name.
        QVERIFY(graph.node(siege)->summary.isEmpty() && graph.node(siege)->aliases().isEmpty());
        QVERIFY(!Chronology::order(graph, siege, "before", mara));
        QVERIFY(!Chronology::order(graph, siege, "long before", graph.upsert({}, "event", "The flood")));
        QVERIFY(!Chronology::placeOn(graph, siege, mara));
        QVERIFY(!Chronology::of(reading.library, mara, GraphContext::readerKnows(reading.library)));
        // Folded into another, the event takes its time along.
        const qint64 fall = graph.upsert({}, "event", "The fall of the city");
        QVERIFY(graph.merge(fall, siege));
        QCOMPARE(times(*Chronology::of(reading.library, fall, GraphContext::readerKnows(reading.library))), QStringList{year.written()});
    }

    void aTimeShowsOnlyOnceItIsRevealed() {
        Reading reading;
        const Story story(reading.library, reading.book);
        reading.readTo(2);
        const auto early = reading.known(story.siege);
        QVERIFY(early.has_value());
        QCOMPARE(times(*early), QStringList{R"({"at":"3019","calendar":"Third Age"})"});
        QCOMPARE(early->latest()->time.precision(), EventTime::Precision::Year);
        reading.readTo(9);
        const auto late = reading.known(story.siege);
        QCOMPARE(late->times.size(), 2);
        QCOMPARE(late->latest()->time.precision(), EventTime::Precision::Day);
        QCOMPARE(late->latest()->revealed.anchor, QString("on the ides"));
        // A reader at a place asked for, as asOf takes it.
        const GraphKnown atThird = [book = reading.book](const GraphPlace &at) { return at.bookId != book || at.notAfter({book, 3, 0}); };
        QCOMPARE(Chronology::of(reading.library, story.siege, atThird)->times.size(), 1);
    }

    void anOrderShowsOnlyOnceItIsRevealedAndOnlyOfEventsMet() {
        Reading reading;
        const Story story(reading.library, reading.book);
        reading.readTo(2);
        QVERIFY(reading.known(story.siege)->order.isEmpty());
        QVERIFY(reading.known(story.flood)->order.isEmpty());
        // Not met yet: nothing of it at all.
        QVERIFY(!reading.known(story.fall));
        reading.readTo(9);
        QCOMPARE(relations(*reading.known(story.siege)), QStringList({"before The fall", "before The flood"}));
        QCOMPARE(relations(*reading.known(story.flood)), QStringList{"after The siege"});
        QCOMPARE(relations(*reading.known(story.fall)), QStringList{"after The siege"});
        // Nothing of a book not begun: back at its start, not 0.9 into its
        // first section, which is past the siege already.
        reading.library.setProgress(reading.book, {});
        QVERIFY(!reading.known(story.siege));
    }

    void aTimelineShowsOnlyOnceAnEventIsPlacedOnIt() {
        Reading reading;
        Story story(reading.library, reading.book);
        reading.readTo(2);
        QCOMPARE(timelines(*reading.known(story.siege)), QStringList{"The Third Age"});
        reading.readTo(5);
        QCOMPARE(timelines(*reading.known(story.siege)), QStringList({"History", "The Third Age"}));
        // Real history is one timeline, whatever another book calls it.
        QCOMPARE(Chronology::timeline(story.graph, Chronology::Whose::History, "Real history"), story.history);
        QVERIFY(Chronology::whose(story.graph.node(story.history)->data.value("timeline").toString()) == Chronology::Whose::History);
        const QJsonObject json = reading.known(story.siege)->toJson();
        QCOMPARE(json.value("timelines").toArray().size(), 2);
        QCOMPARE(json.value("when").toArray().first().toObject().value("time").toObject().value("precision").toString(), QString("year"));
    }

    // A graph someone shared is cut at the receiver's place (#5), its times,
    // orders and timelines too.
    void aSharedGraphsChronologyIsCutAtTheReceiversPlace() {
        QTemporaryDir dir;
        QVERIFY(tenChapters(dir.filePath("siege.epub")));
        Library owner(dir.filePath("Owner"));
        QVERIFY(owner.open());
        const qint64 book = owner.add(dir.filePath("siege.epub")).id;
        owner.setProgress(book, {9, 1.0, 100});
        const auto shelf = owner.addCollection("Sieges");
        QVERIFY(shelf.has_value() && owner.putInCollection(*shelf, book, true));
        const Story story(owner, book);
        const auto snapshot = PublishedSpace::snapshot(owner, *shelf, {.graph = true}, ada());
        QVERIFY2(snapshot.has_value(), qPrintable(Fixtures::whyNot(snapshot)));

        Library receiver(dir.filePath("Receiver"));
        QVERIFY(receiver.open());
        QVERIFY(QFile::copy(dir.filePath("siege.epub"), dir.filePath("mine.epub")));
        const qint64 mine = receiver.add(dir.filePath("mine.epub")).id;
        const auto subscribed = Subscriptions::subscribe(receiver, *snapshot, {});
        QVERIFY2(subscribed.has_value(), qPrintable(Fixtures::whyNot(subscribed)));
        receiver.setProgress(mine, {2, 0.9, 25});
        const GraphLayers::Layer early = GraphContext::layers(receiver).first();
        QVERIFY(named(early, "The siege") && !named(early, "The fall"));
        const auto siege = Chronology::of(early, named(early, "The siege")->id);
        QVERIFY(siege.has_value());
        QCOMPARE(times(*siege), QStringList{R"({"at":"3019","calendar":"Third Age"})"});
        QVERIFY(siege->order.isEmpty());
        QCOMPARE(timelines(*siege), QStringList{"The Third Age"});

        receiver.setProgress(mine, {9, 1.0, 100});
        const GraphLayers::Layer late = GraphContext::layers(receiver).first();
        const auto all = Chronology::of(late, named(late, "The siege")->id);
        QCOMPARE(all->times.size(), 2);
        QCOMPARE(relations(*all), QStringList({"before The fall", "before The flood"}));
        QCOMPARE(timelines(*all), QStringList({"History", "The Third Age"}));
    }

    void aTimeAnOrderAndATimelineTravelThroughTheLog() {
        QTemporaryDir dir;
        QVERIFY(tenChapters(dir.filePath("siege.epub")));
        Library here(dir.filePath("Here"));
        Library there(dir.filePath("There"));
        QVERIFY(here.open() && there.open());
        const qint64 mine = here.add(dir.filePath("siege.epub")).id;
        const qint64 theirs = there.add(dir.filePath("siege.epub")).id;
        here.setProgress(mine, {9, 1.0, 100});
        there.setProgress(theirs, {9, 1.0, 100});
        const Story story(here, mine);
        const auto before = Chronology::of(here, story.siege, GraphContext::readerKnows(here));

        const Remote remote("laptop:" + dir.filePath("There"), {Fixtures::standInSsh(dir.path(), OMABOOK_BINARY)});
        const SyncSummary summary = syncLibrary(here, remote);
        QVERIFY2(summary.problems.isEmpty(), qPrintable(summary.problems.join('\n')));
        const Graph graph(there);
        const qint64 siege = graph.named({}, "event", "The siege");
        QVERIFY(siege > 0);
        const auto after = Chronology::of(there, siege, GraphContext::readerKnows(there));
        QVERIFY(after.has_value());
        QCOMPARE(times(*after), times(*before));
        QCOMPARE(after->latest()->revealed.place.section, 7);
        QCOMPARE(after->latest()->revealed.anchor, QString("on the ides"));
        QCOMPARE(relations(*after), relations(*before));
        QCOMPARE(timelines(*after), timelines(*before));
        QCOMPARE(graph.node(graph.named({}, "timeline", "History"))->data.value("timeline").toString(), QString("history"));
        QCOMPARE(after->timelines.size(), 2);
        QCOMPARE(syncLibrary(here, remote).graphTaken, 0);
    }
};

QTEST_MAIN(ChronologyTests)
#include "chronology_tests.moc"
