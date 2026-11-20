// The chronology read from the books (#42): as the AI builds the graph, it
// notes when the events happen, their order and the timelines they are on.
// What it answers is checked as the chronology checks it (#41): what is no
// time or no order is dropped and counted, never kept, and all of it is
// revealed where the text says it, so a reader is told none of it early.

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

#include "chronology.h"
#include "fixtures.h"
#include "graph.h"
#include "graphbuilder.h"
#include "graphcontext.h"
#include "graphtools.h"
#include "library.h"
#include "prompts.h"

namespace {

// A siege in 1815, a flood three years before it (told in a flashback), and
// the fall of a tower in a calendar of the story's own.
const QStringList siegeChapters{
    "In the spring of 1815 the siege of Hougoumont began. Mara kept watch on the wall.",
    "Three years before the siege, the great flood had come and gone.",
    "In the third year of the Long Night the tower fell, long after the siege.",
};

QList<GraphBuild::Passage> passagesOf(const QStringList &chapters, int first = 0, int last = -1) {
    QList<GraphBuild::Passage> passages;
    for (int section = first; section <= (last < 0 ? chapters.size() - 1 : last); ++section)
        passages.append({section, 0, 1, chapters.at(section)});
    return passages;
}

// What a model answers for the three chapters.
const char *const siegeAnswer = R"({"nodes": [
    {"kind": "event", "name": "The siege", "summary": "The siege of Hougoumont.", "section": 0, "quote": "the siege of Hougoumont began",
     "when": {"calendar": "gregorian", "at": "1815", "approximate": true, "said": "the spring of 1815"}},
    {"kind": "character", "name": "Mara", "summary": "Keeps watch.", "section": 0,
     "when": {"calendar": "gregorian", "at": "1815"}},
    {"kind": "event", "name": "The flood", "summary": "A great flood.", "section": 1, "quote": "the great flood had come",
     "when": {"said": "Three years before the siege"}},
    {"kind": "event", "name": "The fall of the tower", "summary": "The tower fell.", "section": 2, "quote": "the tower fell",
     "when": {"calendar": "the Long Night", "at": "3", "said": "the third year of the Long Night"}, "timeline": "Eldermark"}],
  "edges": [
    {"from": "The flood", "to": "The siege", "relation": "before", "section": 1, "quote": "Three years before the siege"},
    {"from": "The fall of the tower", "to": "The siege", "relation": "after", "section": 2, "inferred": true},
    {"from": "Mara", "to": "The siege", "relation": "before", "section": 0},
    {"from": "Mara", "to": "The siege", "relation": "keeps watch over", "section": 0}]})";

// A library of books of no file, each read as far as a test says.
struct Shelf {
    Shelf() : library(dir.filePath("Library")) { QVERIFY(library.open()); }
    qint64 add(const QString &title, const QString &series = {}) {
        BookMeta meta;
        meta.title = title;
        meta.series = series;
        meta.authors = QStringList{"Ann Author"};
        return library.addEntry(meta, true).id;
    }
    void readTo(qint64 book, int section) { library.setProgress(book, {section, 1.0, (section + 1) * 10.0}); }
    [[nodiscard]] qint64 event(const QString &name) const { return Graph(library).named({}, "event", name); }
    [[nodiscard]] std::optional<Chronology::Known> known(const QString &name) const {
        return Chronology::of(library, event(name), GraphContext::readerKnows(library));
    }

    QTemporaryDir dir;
    Library library;
};

[[nodiscard]] QStringList relations(const Chronology::Known &known) {
    QStringList list;
    for (const Chronology::Ordered &other : known.order)
        list << other.relation + " " + other.other.name + (other.note.isEmpty() ? QString() : " (" + other.note + ")");
    list.sort();
    return list;
}

[[nodiscard]] QStringList timelines(const Chronology::Known &known) {
    QStringList list;
    for (const Chronology::Placed &placed : known.timelines)
        list << placed.timeline.name + ": " + placed.timeline.data.value("timeline").toString();
    list.sort();
    return list;
}

[[nodiscard]] QString answerWith(const QJsonArray &nodes, const QJsonArray &edges = {}) {
    return QString::fromUtf8(QJsonDocument(QJsonObject{{"nodes", nodes}, {"edges", edges}}).toJson(QJsonDocument::Compact));
}

[[nodiscard]] QJsonObject eventNode(const QString &name, const QJsonValue &when, const QJsonValue &timeline = {}) {
    QJsonObject node{{"kind", "event"}, {"name", name}, {"summary", "An omen."}, {"section", 0}};
    if (!when.isUndefined())
        node.insert("when", when);
    if (!timeline.isUndefined())
        node.insert("timeline", timeline);
    return node;
}

} // namespace

class ChronologyBuildTests : public QObject {
    Q_OBJECT

private slots:
    void theModelIsAskedForTimesInFewWords() {
        const AiRequest request = Prompts::graphExtraction(BookMeta(), {}, "<section number=\"0\">\nText.\n</section>");
        QCOMPARE(request.tier, AiRequest::Quick);
        for (const char *taught : {"\"when\"", "\"calendar\"", "\"gregorian\"", "\"said\"", "\"approximate\"", "\"timeline\"",
                                   "\"history\"", "\"series\"", "\"same time as\"", "\"inferred\"", "never guessed"})
            QVERIFY2(request.system.contains(QString::fromUtf8(taught)), taught);
        QVERIFY(Prompts::graphKinds().contains(Chronology::timelineKind()));
    }

    void aChaptersTimesOrdersAndTimelinesLandInTheChronology() {
        Shelf shelf;
        const qint64 book = shelf.add("The Long Siege");
        shelf.readTo(book, 2);
        const GraphBuild::Applied applied = GraphBuild::apply(shelf.library, book, passagesOf(siegeChapters), siegeAnswer);
        QVERIFY(applied.understood);
        QCOMPARE(applied.times.times, 3);
        QCOMPARE(applied.times.orders, 2);
        // The siege on the book's own timeline and, dated on the real
        // calendar, on real history; the flood on the book's; the fall on
        // the world's the model named.
        QCOMPARE(applied.times.placed, 4);
        // Mara is no event: her time and her order are none.
        QCOMPARE(applied.times.dropped, 2);
        QCOMPARE(applied.edges, 1);

        const auto siege = shelf.known("The siege");
        QVERIFY(siege.has_value());
        QCOMPARE(siege->times.size(), 1);
        const EventTime time = siege->latest()->time;
        QVERIFY(time.on.gregorian() && time.approximate);
        QCOMPARE(time.precision(), EventTime::Precision::Year);
        QCOMPARE(time.said, QString("the spring of 1815"));
        // Revealed where the book says it.
        QCOMPARE(siege->latest()->revealed.place.section, 0);
        QCOMPARE(siege->latest()->revealed.anchor, QString("the spring of 1815"));
        QCOMPARE(relations(*siege), QStringList({"after The flood", "before The fall of the tower (inferred)"}));
        QCOMPARE(timelines(*siege), QStringList({"Real history: history", "The Long Siege: book"}));

        const auto flood = shelf.known("The flood");
        QCOMPARE(flood->latest()->time.precision(), EventTime::Precision::None);
        QCOMPARE(flood->latest()->time.said, QString("Three years before the siege"));
        QCOMPARE(timelines(*flood), QStringList{"The Long Siege: book"});
        const auto fall = shelf.known("The fall of the tower");
        QCOMPARE(fall->latest()->time.on.calendar, QString("the Long Night"));
        QCOMPARE(timelines(*fall), QStringList{"Eldermark: world"});
        // The same answer again changes nothing.
        QCOMPARE(GraphBuild::apply(shelf.library, book, passagesOf(siegeChapters), siegeAnswer).times.dropped, 2);
        QCOMPARE(shelf.known("The siege")->times.size(), 1);
        QCOMPARE(shelf.known("The siege")->order.size(), 2);
        // What the model gives as a timeline of its own is not taken: a
        // timeline is made from its events.
        const auto noTimeline = GraphBuild::apply(shelf.library, book, passagesOf(siegeChapters, 0, 0),
                                                  R"({"nodes": [{"kind": "timeline", "name": "Made up", "section": 0}]})");
        QCOMPARE(noTimeline.nodes, 0);
    }

    void whatIsNoTimeIsDroppedAndCounted() {
        Shelf shelf;
        const qint64 book = shelf.add("Omens");
        shelf.readTo(book, 0);
        const QStringList chapter{"The first omen came, then the second omen, and more omens after."};
        const QJsonArray nodes{
            eventNode("The first omen", "1815"),
            eventNode("The second omen", QJsonObject{{"calendar", "gregorian"}, {"at", "1815-13-01"}}),
            eventNode("The third omen", QJsonObject{{"at", "1815"}}),
            eventNode("The fourth omen", QJsonObject{{"calendar", "gregorian"}, {"at", "1815"}, {"from", "1814"}}),
            eventNode("The fifth omen", QJsonObject{{"calendar", "gregorian"}, {"at", "1815"}, {"when", "soon"}}),
            // Left as the answer's template has it: no time, and nothing wrong.
            eventNode("The sixth omen", QJsonObject{{"calendar", ""}, {"at", ""}, {"said", ""}, {"approximate", false}}),
            eventNode("The seventh omen", QJsonValue::Undefined, 5),
        };
        const QJsonArray edges{
            QJsonObject{{"from", "The first omen"}, {"to", "Nobody at all"}, {"relation", "before"}},
            QJsonObject{{"from", "The first omen"}, {"to", "The first omen"}, {"relation", "after"}},
            QJsonObject{{"from", "The first omen"}, {"to", "The second omen"}, {"relation", "Same time as"}},
        };
        const GraphBuild::Applied applied = GraphBuild::apply(shelf.library, book, passagesOf(chapter), answerWith(nodes, edges));
        QVERIFY(applied.understood);
        QCOMPARE(applied.nodes, 7);
        QCOMPARE(applied.times.times, 0);
        QCOMPARE(applied.times.orders, 1);
        QCOMPARE(applied.times.dropped, 5 + 1 + 2);
        // Every event is still on its book's timeline.
        QCOMPARE(applied.times.placed, 7);
        // An edge of order is no other relation either.
        QCOMPARE(applied.edges, 0);
        const Graph graph(shelf.library);
        for (const QJsonValue &node : nodes) {
            const qint64 id = graph.named({}, "event", node.toObject().value("name").toString());
            QVERIFY(id > 0);
            QVERIFY(std::ranges::none_of(graph.facts(id), [](const GraphFact &fact) { return fact.kind == Chronology::whenFact(); }));
        }
        QCOMPARE(relations(*shelf.known("The second omen")), QStringList{"same time as The first omen"});
    }

    void nothingPastTheReadersPlaceShows() {
        Shelf shelf;
        const qint64 book = shelf.add("The Long Siege");
        QVERIFY(GraphBuild::apply(shelf.library, book, passagesOf(siegeChapters), siegeAnswer).understood);
        shelf.readTo(book, 0);
        const auto siege = shelf.known("The siege");
        QVERIFY(siege.has_value());
        QCOMPARE(siege->times.size(), 1);
        QCOMPARE(timelines(*siege), QStringList({"Real history: history", "The Long Siege: book"}));
        // The flashback is in the next chapter, the fall in the last.
        QVERIFY(siege->order.isEmpty());
        QVERIFY(!shelf.known("The flood"));
        QVERIFY(!shelf.known("The fall of the tower"));

        shelf.readTo(book, 1);
        QCOMPARE(relations(*shelf.known("The siege")), QStringList{"after The flood"});
        QCOMPARE(shelf.known("The flood")->times.size(), 1);
        QVERIFY(!shelf.known("The fall of the tower"));
        shelf.readTo(book, 2);
        QCOMPARE(shelf.known("The siege")->order.size(), 2);
        QCOMPARE(timelines(*shelf.known("The fall of the tower")), QStringList{"Eldermark: world"});
    }

    void aBuildNotesTimesNoFurtherThanItWasAsked() {
        Shelf shelf;
        QList<Fixtures::Chapter> chapters;
        for (const QString &text : siegeChapters)
            chapters << Fixtures::Chapter{"Chapter", "<p>" + text + "</p>"};
        QVERIFY(Fixtures::chapteredEpub(shelf.dir.filePath("siege.epub"), "The Long Siege", chapters));
        const qint64 book = shelf.library.add(shelf.dir.filePath("siege.epub")).id;
        QVERIFY(book > 0);
        // The model answers for all three chapters, though only the first was sent.
        const GraphBuild::Asker ask = [](const AiRequest &, QString *answer, QString *) {
            *answer = siegeAnswer;
            return true;
        };
        const GraphPlace goal{book, 0, 1.0};
        const QJsonObject built = GraphOps::build(shelf.library, book, goal, false, false, ask);
        QCOMPARE(built.value("chronology").toObject().value("times").toInt(), 3);
        QCOMPARE(built.value("chronology").toObject().value("dropped").toInt(), 2);
        const Graph graph(shelf.library);
        for (const char *name : {"The siege", "The flood", "The fall of the tower"}) {
            const qint64 id = graph.named({}, "event", name);
            for (const GraphFact &fact : graph.facts(id))
                QVERIFY2(fact.revealed.place.notAfter(goal), name);
            for (const GraphEdge &edge : graph.around(id, 1).edges)
                QVERIFY2(edge.learned.notAfter(goal), qPrintable(edge.kind));
        }
    }

    // A series' or a world's timeline is shared by its books on one side of
    // the library, never across top collections kept apart.
    void aSharedTimelineKeepsToItsSideOfTheLibrary() {
        Shelf shelf;
        const QString series = "The Eldermark Cycle";
        const qint64 first = shelf.add("The Crowning", series), second = shelf.add("The War", series);
        const qint64 third = shelf.add("The Thaw", series), fourth = shelf.add("The Spring", series);
        const auto play = shelf.library.addCollection("Spel");
        const auto work = shelf.library.addCollection("Werk");
        QVERIFY(play.has_value() && work.has_value());
        for (const qint64 book : {first, second})
            QVERIFY(shelf.library.putInCollection(*play, book, true));
        for (const qint64 book : {third, fourth})
            QVERIFY(shelf.library.putInCollection(*work, book, true));
        const QStringList text{"The crowning was held. The war began. The thaw came. The spring returned."};
        const auto lay = [&](qint64 book, const QString &event) {
            shelf.readTo(book, 0);
            const QJsonArray nodes{eventNode(event, QJsonValue::Undefined, "series"),
                                   eventNode(event + " ended", QJsonValue::Undefined, "Eldermark")};
            QCOMPARE(GraphBuild::apply(shelf.library, book, passagesOf(text), answerWith(nodes)).times.placed, 2);
        };
        lay(first, "The crowning");
        lay(second, "The war");
        lay(third, "The thaw");
        lay(fourth, "The spring");

        QCOMPARE(timelines(*shelf.known("The crowning")), QStringList{"The Eldermark Cycle: series"});
        QCOMPARE(timelines(*shelf.known("The war")), QStringList{"The Eldermark Cycle: series"});
        QCOMPARE(timelines(*shelf.known("The thaw")), QStringList{"The Eldermark Cycle (Ann Author): series"});
        QCOMPARE(timelines(*shelf.known("The spring")), QStringList{"The Eldermark Cycle (Ann Author): series"});
        QCOMPARE(timelines(*shelf.known("The war ended")), QStringList{"Eldermark: world"});
        QCOMPARE(timelines(*shelf.known("The spring ended")), QStringList{"Eldermark (The Eldermark Cycle): world"});
        // A book of no series has a timeline of its own for "series".
        const qint64 alone = shelf.add("Alone");
        shelf.readTo(alone, 0);
        QVERIFY(GraphBuild::apply(shelf.library, alone, passagesOf(text),
                                  answerWith({eventNode("The thaw came", QJsonValue::Undefined, "series")}))
                    .understood);
        QCOMPARE(timelines(*shelf.known("The thaw came")), QStringList{"Alone: book"});
    }
};

QTEST_MAIN(ChronologyBuildTests)
#include "chronologybuild_tests.moc"
