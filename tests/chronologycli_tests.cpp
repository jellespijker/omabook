// The chronology on the command line and the MCP server (#45): the real
// binary, as a person in a terminal and an agent meet it. A book read to
// section 5 with dates, a span, an order, events not placed, a
// contradiction, and an event past the reader that is never told.

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>
#include <algorithm>
#include <ranges>

#include "chronology.h"
#include "eventtime.h"
#include "graph.h"
#include "library.h"
#include "mcpharness.h"

using McpHarness::Machine;
using McpHarness::Server;

namespace {

[[nodiscard]] EventTime timeOf(const QString &json) {
    const auto time = EventTime::parse(json);
    if (!time)
        qFatal("%s", qPrintable(time.error()));
    return *time;
}

// A book, its graph, and its reader at section 5.
struct Story {
    Machine machine;
    qint64 book = 0, war = 0;

    Story() {
        Library library(machine.library);
        if (!library.open())
            qFatal("no library");
        BookMeta meta;
        meta.title = "The Long Siege";
        meta.series = "The War Years";
        book = library.addEntry(meta, true).id;
        library.setProgress(book, {5, 0.9, 50.0});
        Graph graph(library);
        const auto at = [this](int section) { return GraphMoment{{book, section, 0.5}, QString("in chapter %1").arg(section)}; };
        const auto event = [&](const char *name, int section) {
            return graph.upsert({}, "event", name, {}, {}, GraphPlace{book, section, 0.1});
        };
        war = Chronology::timeline(graph, Chronology::Whose::World, "The War", at(0));
        const qint64 siege = event("The siege", 0), treaty = event("The treaty", 0), flood = event("The flood", 1);
        const qint64 fall = event("The fall", 1), rise = event("The rise", 1);
        const qint64 meeting = event("The meeting", 2), legend = event("The legend", 2), end = event("The end", 8);
        bool kept = Chronology::setWhen(graph, siege, timeOf(R"({"calendar": "gregorian", "at": "1942"})"), at(0)).has_value();
        kept =
            kept
            && Chronology::setWhen(graph, treaty, timeOf(R"({"calendar": "gregorian", "from": "1939-09-01", "to": "1945-09-02"})"), at(0));
        kept = kept && Chronology::setWhen(graph, legend, timeOf(R"({"said": "long ago"})"), at(2));
        kept = kept && Chronology::setWhen(graph, end, timeOf(R"({"calendar": "gregorian", "at": "1950"})"), at(8));
        kept = kept && Chronology::order(graph, flood, "before", siege, at(1));
        // A contradiction: the fall before the rise, then the rise before the fall.
        kept = kept && Chronology::order(graph, fall, "before", rise, at(2));
        kept = kept && Chronology::order(graph, rise, "before", fall, at(3));
        for (const qint64 placed : {siege, treaty, flood, end})
            kept = kept && Chronology::placeOn(graph, placed, war, at(0));
        if (!kept || meeting <= 0)
            qFatal("the story could not be kept");
    }

    [[nodiscard]] Machine::Ran run(QStringList arguments) const {
        arguments.prepend("chronology");
        return machine.omabook(arguments << "--library" << machine.library);
    }
    [[nodiscard]] QJsonObject json(const QStringList &arguments = {}) const {
        return QJsonDocument::fromJson(run(QStringList(arguments) << "--json").output.toUtf8()).object();
    }
};

[[nodiscard]] QJsonObject laneNamed(const QJsonObject &answer, const QString &name) {
    for (const QJsonValue &lane : answer.value("parts").toArray().first().toObject().value("lanes").toArray())
        if (lane.toObject().value("timeline").toObject().value("name").toString() == name)
            return lane.toObject();
    return {};
}

[[nodiscard]] QJsonObject itemNamed(const QJsonObject &lane, const QString &name) {
    for (const QJsonValue &item : lane.value("items").toArray())
        if (item.toObject().value("name").toString() == name)
            return item.toObject();
    return {};
}

[[nodiscard]] QStringList namesIn(const QJsonObject &lane) {
    QStringList names;
    for (const QJsonValue &item : lane.value("items").toArray())
        names << item.toObject().value("name").toString();
    return names;
}

} // namespace

class ChronologyCliTests : public QObject {
    Q_OBJECT

private slots:
    void theTextReadsInATerminal() {
        const Story story;
        const auto ran = story.run({});
        QCOMPARE(ran.code, 0);
        const QString text = ran.output;
        QVERIFY2(text.contains("The War (world), on the Gregorian calendar"), qPrintable(text));
        QVERIFY2(text.contains("1942") && text.contains("The siege") && text.contains("The Long Siege, section 0"), qPrintable(text));
        QVERIFY2(text.contains("1939-09-01 – 1945-09-02"), qPrintable(text));
        QVERIFY2(text.contains("Not placed yet") && text.contains("The meeting") && text.contains("“long ago”"), qPrintable(text));
        // The contradiction, with where each side of it was said, and which is set aside.
        QVERIFY2(text.contains("The fall before The rise  (The Long Siege, section 2)  set aside"), qPrintable(text));
        QVERIFY2(text.contains("The rise before The fall  (The Long Siege, section 3)"), qPrintable(text));
        QVERIFY2(text.contains("Cut where you are in each book."), qPrintable(text));
        // Past the reader: never told.
        QVERIFY2(!text.contains("The end") && !text.contains("1950"), qPrintable(text));
    }

    void theJsonHasTheLanesInOrder() {
        const Story story;
        const QJsonObject answer = story.json();
        QCOMPARE(answer.value("cut").toObject().value("at").toString(), QString("reader"));
        QCOMPARE(answer.value("parts").toArray().size(), 1);
        const QJsonObject war = laneNamed(answer, "The War");
        QCOMPARE(war.value("calendar").toString(), QString("gregorian"));
        QVERIFY(itemNamed(war, "The flood").value("rank").toInt() < itemNamed(war, "The siege").value("rank").toInt());
        QCOMPARE(itemNamed(war, "The siege").value("time").toObject().value("at").toString(), QString("1942"));
        QCOMPARE(itemNamed(war, "The treaty").value("time").toObject().value("from").toString(), QString("1939-09-01"));
        QVERIFY(!namesIn(war).contains("The end"));
        const QJsonObject part = answer.value("parts").toArray().first().toObject();
        QCOMPARE(part.value("unplaced").toArray().size(), 2);
        QJsonArray contradictions;
        for (const QJsonValue &lane : part.value("lanes").toArray())
            for (const QJsonValue &contradiction : lane.toObject().value("contradictions").toArray())
                contradictions << contradiction;
        QCOMPARE(contradictions.size(), 1);
        QCOMPARE(contradictions.first().toObject().value("kind").toString(), QString("cycle"));
        QCOMPARE(contradictions.first().toObject().value("sources").toArray().size(), 2);
        QCOMPARE(answer.value("books").toObject().value(QString::number(story.book)).toString(), QString("The Long Siege"));
        QCOMPARE(answer.value("events").toObject().value("total").toInt(), 7);
        QCOMPARE(answer.value("events").toObject().value("shown").toInt(), 7);
    }

    void itIsCutWhereAsked() {
        const Story story;
        const QJsonObject whole = story.json({"--whole"});
        QVERIFY(namesIn(laneNamed(whole, "The War")).contains("The end"));
        QCOMPARE(whole.value("cut").toObject().value("at").toString(), QString("whole"));
        // At section 2 the rise before the fall isn't said yet: no contradiction, no rise.
        const QJsonObject early = story.json({QString::number(story.book), "--upto", "2"});
        QCOMPARE(early.value("cut").toObject().value("section").toInt(), 2);
        QVERIFY(!QJsonDocument(early).toJson().contains("\"cycle\""));
        QVERIFY(!QJsonDocument(early).toJson().contains("The end"));
        QVERIFY(story.run({"--upto", "2"}).errors.contains("name the book"));
        QVERIFY(story.run({"--whole", QString::number(story.book), "--upto", "2"}).code == 1);
        // A book, a series: the same events; a book that isn't there is refused.
        QCOMPARE(story.json({QString::number(story.book)}).value("events").toObject().value("total").toInt(), 7);
        QCOMPARE(story.json({"--series", "The War Years"}).value("events").toObject().value("total").toInt(), 7);
        QVERIFY(story.run({"--series", "Nowhere"}).errors.contains("No book of the library is in a series"));
        QVERIFY(story.run({"99"}).errors.contains("There is no book 99"));
    }

    void aTimelineAndAStretchOfTimeNarrowIt() {
        const Story story;
        const QJsonObject war = story.json({"--timeline", "The War"});
        QCOMPARE(war.value("parts").toArray().first().toObject().value("lanes").toArray().size(), 1);
        QVERIFY(war.value("parts").toArray().first().toObject().value("unplaced").toArray().isEmpty());
        QCOMPARE(war.value("events").toObject().value("shown").toInt(), 3);
        QCOMPARE(war.value("events").toObject().value("total").toInt(), 7);
        QVERIFY(story.run({"--timeline", "The War"}).output.contains("3 of 7 events."));
        QVERIFY(story.run({"--timeline", "Nowhere"}).errors.contains("There is no timeline called"));

        const QJsonObject stretch = story.json({"--from", "1943", "--to", "1949"});
        const QStringList names = namesIn(laneNamed(stretch, "The War"));
        QVERIFY2(names.contains("The treaty") && !names.contains("The siege"), qPrintable(names.join(", ")));
        QVERIFY(stretch.value("parts").toArray().first().toObject().value("unplaced").toArray().isEmpty());
        QVERIFY(story.run({"--from", "soon"}).errors.contains("not a time"));
    }

    void timesOrdersAndTimelinesArePutInByHand() {
        const Story story;
        auto ran = story.run({"set-when", "The meeting", "1941-05", "--approximate"});
        QVERIFY2(ran.code == 0 && ran.output.contains("The meeting: about 1941-05, by hand."), qPrintable(ran.output + ran.errors));
        // Put in by hand, it wins over what the book said, and is marked so.
        QCOMPARE(story.run({"set-when", "The siege", "1944"}).code, 0);
        QJsonObject siege = itemNamed(laneNamed(story.json(), "The War"), "The siege");
        QCOMPARE(siege.value("time").toObject().value("at").toString(), QString("1944"));
        QVERIFY(siege.value("by_hand").toBool());
        QVERIFY(story.run({"set-when", "The meeting", "1941-13"}).errors.contains("month"));
        QVERIFY(story.run({"set-when", "Nobody", "1941"}).errors.contains("There is no event called"));
        QVERIFY(story.run({"set-when", QString::number(story.war), "1941"}).errors.contains("is a timeline, not a event"));

        ran = story.run({"order", "The meeting", "same", "The siege"});
        QVERIFY2(ran.code == 0 && ran.output.contains("The meeting same time as The siege, by hand."), qPrintable(ran.output + ran.errors));
        QVERIFY(story.run({"order", "The meeting", "sideways", "The siege"}).errors.contains("no relation of order"));
        QVERIFY(story.run({"order", "The meeting", "before", "The meeting"}).errors.contains("no order with itself"));

        ran = story.run({"timeline", "add", "The Shire", "--kind", "world"});
        QVERIFY2(ran.code == 0 && ran.output.contains("The Shire (world)"), qPrintable(ran.output + ran.errors));
        QVERIFY(story.run({"timeline", "add", "Elsewhere", "--kind", "nonsense"}).errors.contains("no kind of timeline"));
        ran = story.run({"place", "The meeting", "on", "The Shire"});
        QVERIFY2(ran.code == 0 && ran.output.contains("The meeting is on The Shire, by hand."), qPrintable(ran.output + ran.errors));
        QVERIFY(story.run({"place", "The meeting", "on", "Nowhere"}).errors.contains("timeline add"));
        QVERIFY(!itemNamed(laneNamed(story.json(), "The Shire"), "The meeting").isEmpty());
        QVERIFY(story.run({"place", "The meeting"}).code == 1);
    }

    void itIsWrittenAsAFileOtherToolsRead() {
        const Story story;
        // CSV: a row per placed event on each lane, the time as said and where.
        auto ran = story.run({"--format", "csv"});
        QCOMPARE(ran.code, 0);
        const QStringList rows = ran.output.trimmed().split('\n');
        QVERIFY2(rows.first().startsWith("timeline,timeline_id,calendar,rank,event,event_id,time,extent_from,extent_to,book,section"),
                 qPrintable(rows.first()));
        // The War's three, and the rise and the fall in the book's own order.
        QCOMPARE(rows.size(), 1 + 5);
        QVERIFY2(std::ranges::any_of(rows,
                                     [](const QString &row) {
                                         return row.startsWith("The War,") && row.contains(",The siege,") && row.contains(",1942,")
                                                && row.contains(",The Long Siege,0,");
                                     }),
                 qPrintable(ran.output));
        QVERIFY2(ran.output.contains("1939-09-01 – 1945-09-02"), qPrintable(ran.output));
        QVERIFY(!ran.output.contains("The end"));
        // dot and graphml, with the order as edges.
        ran = story.run({"--format", "dot", "--timeline", "The War"});
        QVERIFY2(ran.output.startsWith("digraph omabook") && ran.output.contains("[label=\"before\"]")
                     && ran.output.contains("[label=\"on\"]"),
                 qPrintable(ran.output));
        const QString file = story.machine.dir.filePath("chronology.graphml");
        ran = story.run({"--output", file});
        QVERIFY2(ran.code == 0 && ran.output.contains("as graphml"), qPrintable(ran.output + ran.errors));
        QFile written(file);
        QVERIFY(written.open(QIODevice::ReadOnly));
        const QByteArray graphml = written.readAll();
        QVERIFY(graphml.contains("<graphml") && graphml.contains("The siege") && graphml.contains(">before<"));
        QCOMPARE(QJsonDocument::fromJson(story.run({"--format", "json"}).output.toUtf8()).object().value("events"),
                 story.json().value("events"));
        QVERIFY(story.run({"--format", "pdf"}).errors.contains("no format of the chronology"));
    }

    // `omabook graph chronology` is the same command, as the issue named it.
    void theGraphCommandTakesItToo() {
        const Story story;
        const auto ran = story.machine.omabook({"graph", "chronology", "--json", "--library", story.machine.library});
        QCOMPARE(ran.code, 0);
        QCOMPARE(QJsonDocument::fromJson(ran.output.toUtf8()).object(), story.json());
        const auto before = story.machine.omabook({"graph", "--library", story.machine.library, "chronology", "--timeline", "The War"});
        QVERIFY2(before.output.contains("3 of 7 events."), qPrintable(before.output + before.errors));
        const auto set =
            story.machine.omabook({"graph", "chronology", "set-when", "The meeting", "1941", "--library", story.machine.library});
        QVERIFY2(set.code == 0 && set.output.contains("The meeting: 1941, by hand."), qPrintable(set.output + set.errors));
        QVERIFY(story.machine.omabook({"graph", "--help"}).output.contains("chronology"));
        QVERIFY(story.machine.omabook({"chronology", "--help"}).output.contains("omabook graph chronology"));
        // The rest of the graph's commands are as they were.
        QCOMPARE(story.machine.omabook({"graph", "scopes", "--library", story.machine.library}).code, 0);
    }

    // A timeline that branches off another at an event (#171), by hand and
    // through the tools: the same up to it, its own after it.
    void aTimelineBranchesOnTheCommandLineAndOverMcp() {
        const Story story;
        auto ran = story.run({"timeline", "branch", "The Peace", "--from", "The War", "--at", "The siege"});
        QVERIFY2(ran.code == 0 && ran.output.contains("The Peace branches off The War at The siege, by hand."),
                 qPrintable(ran.output + ran.errors));
        QCOMPARE(story.run({"set-when", "The meeting", "1943"}).code, 0);
        QCOMPARE(story.run({"place", "The meeting", "on", "The Peace"}).code, 0);
        const QJsonObject peace = laneNamed(story.json(), "The Peace");
        QCOMPARE(peace.value("branch").toObject().value("from_name").toString(), QString("The War"));
        QVERIFY(itemNamed(peace, "The siege").value("shared").toBool());
        QVERIFY(!itemNamed(peace, "The meeting").isEmpty() && !itemNamed(peace, "The meeting").value("shared").toBool());
        QVERIFY(itemNamed(laneNamed(story.json(), "The War"), "The meeting").isEmpty());
        ran = story.run({});
        QVERIFY2(ran.output.contains("The Peace (branch), on the Gregorian calendar, branching off The War at The siege"),
                 qPrintable(ran.output));
        QVERIFY(story.run({"timeline", "branch", "The Truce", "--from", "The War", "--at", "Nobody"}).errors.contains("no event called"));
        QVERIFY(story.run({"timeline", "branch", "The Truce", "--from", "The War"}).errors.contains("--at"));
        QVERIFY(
            story.run({"timeline", "branch", "The War", "--from", "The Peace", "--at", "The siege"}).errors.contains("branch off itself"));
        QVERIFY(story.run({"timeline", "accept", "The War"}).errors.contains("branches off no timeline"));
        QVERIFY(story.run({"timeline", "add", "Elsewhere", "--kind", "branch"}).errors.contains("timeline branch"));

        Server server(story.machine);
        QVERIFY(server.data("chronology_timeline_branch", {{"name", "The Truce"}, {"from", "The War"}, {"at", "The flood"}})
                    .value("timeline")
                    .toInteger()
                > 0);
        QVERIFY(server.data("chronology", {{"timeline", "The Truce"}}).value("events").toObject().value("shown").toInt() > 0);
        QCOMPARE(server.data("chronology_timeline_decide", {{"timeline", "The Truce"}, {"word", "refuse"}}).value("standing").toString(),
                 QString("refused"));
        QVERIFY(laneNamed(story.json(), "The Truce").isEmpty());
        QVERIFY2(server.noise().isEmpty(), qPrintable(server.noise().join('\n')));
    }

    void theToolsAreTheCommands() {
        const Story story;
        Server server(story.machine);
        const QJsonObject read = server.data("chronology", {{"timeline", "The War"}});
        QCOMPARE(read.value("events").toObject().value("shown").toInt(), 3);
        QVERIFY(!QJsonDocument(server.data("chronology")).toJson().contains("The end"));
        QVERIFY(QJsonDocument(server.data("chronology", {{"whole", true}})).toJson().contains("The end"));
        QCOMPARE(server.data("chronology", {{"book", story.book}, {"up_to_section", 2}}).value("cut").toObject().value("section").toInt(),
                 2);

        QVERIFY(server.data("chronology_set_when", {{"event", "The meeting"}, {"time", "1941"}}).value("by_hand").toBool());
        QVERIFY(server.failure("chronology_set_when", {{"event", "The meeting"}, {"time", "soon"}}).contains("not a time"));
        QCOMPARE(server.data("chronology_order", {{"event", "The meeting"}, {"relation", "before"}, {"other", "The siege"}})
                     .value("relation")
                     .toString(),
                 QString("before"));
        QVERIFY(server.failure("chronology_order", {{"event", "The meeting"}, {"relation", "before"}, {"other", "Nobody"}})
                    .contains("no event"));
        QVERIFY(server.data("chronology_timeline_add", {{"name", "The Shire"}, {"kind", "world"}}).value("timeline").toInteger() > 0);
        QCOMPARE(server.data("chronology_place", {{"event", "The meeting"}, {"timeline", "The Shire"}}).value("timeline_name").toString(),
                 QString("The Shire"));
        QVERIFY2(server.noise().isEmpty(), qPrintable(server.noise().join('\n')));
    }

    // Where the library is kept apart by its top collections, each is a part of
    // its own, and a collection asks for its side alone.
    void eachSideOfTheLibraryIsAPartOfItsOwn() {
        const Story story;
        {
            Library library(story.machine.library);
            QVERIFY(library.open());
            BookMeta meta;
            meta.title = "The Other Shore";
            const qint64 other = library.addEntry(meta, true).id;
            library.setProgress(other, {5, 0.9, 50.0});
            const auto study = library.addCollection("Study");
            const auto leisure = library.addCollection("Leisure");
            QVERIFY(study && leisure);
            QVERIFY(library.putInCollection(*study, story.book, true));
            QVERIFY(library.putInCollection(*leisure, other, true));
            Graph graph(library);
            const qint64 landing = graph.upsert({}, "event", "The landing", {}, {}, GraphPlace{other, 0, 0.1});
            QVERIFY(Chronology::setWhen(graph, landing, timeOf(R"({"calendar": "gregorian", "at": "1944"})"),
                                        GraphMoment{{other, 0, 0.5}, {}}));
            QVERIFY(Chronology::placeOn(graph, landing, graph.named(QString(""), "timeline", "The War"), GraphMoment{{other, 0, 0.5}, {}}));
        }
        const QJsonObject both = story.json();
        QCOMPARE(both.value("parts").toArray().size(), 2);
        const QJsonObject study = story.json({"--collection", "Study"});
        QCOMPARE(study.value("parts").toArray().size(), 1);
        QVERIFY(!QJsonDocument(study).toJson().contains("The landing"));
        QVERIFY(story.run({}).output.contains("Of the books in Study:"));
        QVERIFY(story.run({"--collection", "Nowhere"}).errors.contains("There is no collection called Nowhere"));
    }
};

QTEST_GUILESS_MAIN(ChronologyCliTests)
#include "chronologycli_tests.moc"
