// Timelines that branch (#171): a timeline the same as another up to an
// event and its own after it. An alternate-history book stays off real
// history, and the AI only proposes its branch, for the reader to take or
// not; the events before the split show on both; a split, and what comes
// after it, show only once read; a branch branches again and never off
// itself; a story's branch made by a plugin (as Storyteller does); and a
// branch goes to one's other computer and into a space someone subscribes to.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <memory>
#include <ranges>

#include "chronicle.h"
#include "chronology.h"
#include "chronologybranch.h"
#include "chronologychart.h"
#include "chronologymodel.h"
#include "chronologyproposals.h"
#include "eventtime.h"
#include "fixtures.h"
#include "graph.h"
#include "graphbuilder.h"
#include "graphcontext.h"
#include "library.h"
#include "person.h"
#include "publishedspace.h"
#include "remote.h"
#include "scriptplugin.h"
#include "scriptplugins.h"
#include "subscriptions.h"
#include "sync.h"
#include "tenancy.h"

using Chronology::Chronicle;
using Chronology::Lane;

namespace {

[[nodiscard]] EventTime timeOf(const QString &json) {
    const auto time = EventTime::parse(json);
    if (!time)
        qFatal("%s", qPrintable(time.error()));
    return *time;
}

// The chronicle of a library's one tenant, as its reader knows it.
[[nodiscard]] Chronicle chronicleOf(const Library &library) {
    return Chronology::chronicle(Chronology::read(library, GraphContext::readerKnows(library), Chronology::tenants(library).first()));
}

[[nodiscard]] const Lane *laneNamed(const Chronicle &chronicle, const QString &name) {
    const auto found = std::ranges::find(chronicle.lanes, name, [](const Lane &lane) { return lane.timeline.name; });
    return found == chronicle.lanes.cend() ? nullptr : &*found;
}

[[nodiscard]] qsizetype laneAt(const Chronicle &chronicle, const QString &name) {
    return std::ranges::find(chronicle.lanes, name, [](const Lane &lane) { return lane.timeline.name; }) - chronicle.lanes.cbegin();
}

[[nodiscard]] QStringList names(const Lane *lane) {
    QStringList list;
    if (lane)
        for (const Chronology::Item &item : lane->items)
            list << item.event.name;
    return list;
}

[[nodiscard]] QStringList sharedOf(const Lane *lane) {
    QStringList list;
    if (lane)
        for (const Chronology::Item &item : lane->items)
            if (lane->shared.contains(item.event.id))
                list << item.event.name;
    return list;
}

// Every event on any lane of a timeline of that name; none when there is no such lane.
[[nodiscard]] bool onAnyLane(const Chronicle &chronicle, const QString &event) {
    return std::ranges::any_of(chronicle.lanes, [&event](const Lane &lane) { return names(&lane).contains(event); });
}

// A library of books of no file: a novel of the real war, and one in which
// the war goes another way, each with its chapters, and what a model answers
// of each, as the graph is built.
const QStringList warChapters{
    "In 1940 the invasion came over the border.",
    "In 1945 the armistice was signed.",
};
const char *const warAnswer = R"({"nodes": [
    {"kind": "event", "name": "The invasion", "section": 0, "quote": "the invasion came", "timeline": "history",
     "when": {"calendar": "gregorian", "at": "1940"}},
    {"kind": "event", "name": "The armistice", "section": 1, "quote": "the armistice was signed", "timeline": "history",
     "when": {"calendar": "gregorian", "at": "1945"}}], "edges": []})";

const QStringList otherwiseChapters{
    "In the winter of 1941 the siege of Moscow began.",
    "In this world the siege of Moscow ended otherwise, and the war was lost.",
    "In 1946 the victory parade went through Berlin. By 1950 the new order ruled.",
};
const char *const otherwiseAnswer = R"({"nodes": [
    {"kind": "event", "name": "The siege of Moscow", "section": 0, "quote": "the siege of Moscow began", "timeline": "history",
     "when": {"calendar": "gregorian", "at": "1941"}},
    {"kind": "timeline", "name": "Alternate history from the siege of Moscow", "branches_from": "history",
     "at": "The siege of Moscow", "section": 1, "quote": "the siege of Moscow ended otherwise"},
    {"kind": "event", "name": "The victory parade", "section": 2, "quote": "the victory parade went",
     "timeline": "Alternate history from the siege of Moscow", "when": {"calendar": "gregorian", "at": "1946"}},
    {"kind": "event", "name": "The new order", "section": 2, "quote": "the new order ruled",
     "when": {"calendar": "gregorian", "at": "1950"}}], "edges": []})";
const QString branchName = "Alternate history from the siege of Moscow";

[[nodiscard]] QList<GraphBuild::Passage> passagesOf(const QStringList &chapters) {
    QList<GraphBuild::Passage> passages;
    for (int section = 0; section < chapters.size(); ++section)
        passages.append({section, 0, 1, chapters.at(section)});
    return passages;
}

struct Shelf {
    // Read to the end of each, unless only to `upTo` in the alternate history (the furthest place read stays known).
    explicit Shelf(int upTo = 2) : library(dir.filePath("Library")) {
        QVERIFY(library.open());
        war = add("The Long War");
        otherwise = add("Fatherland");
        QVERIFY(GraphBuild::apply(library, war, passagesOf(warChapters), warAnswer).understood);
        const GraphBuild::Applied applied = GraphBuild::apply(library, otherwise, passagesOf(otherwiseChapters), otherwiseAnswer);
        QCOMPARE(applied.times.proposed, 1);
        readTo(war, 1);
        if (upTo >= 2)
            readTo(otherwise, 2);
        else
            library.setProgress(otherwise, {upTo, 0.5, 5});
    }
    qint64 add(const QString &title) {
        BookMeta meta;
        meta.title = title;
        return library.addEntry(meta, true).id;
    }
    void readTo(qint64 book, int section) { library.setProgress(book, {section, 1.0, (section + 1) * 10.0}); }
    [[nodiscard]] qint64 named(const QString &kind, const QString &name) const { return Graph(library).named({}, kind, name); }
    [[nodiscard]] qint64 branch() const { return named("timeline", branchName); }

    QTemporaryDir dir;
    Library library;
    qint64 war = 0, otherwise = 0;
};

// A world of the story's own, by hand: three events on The Realm, a branch
// of it at the second, and a branch of that branch at its own first.
struct Realm {
    Realm() : library(dir.filePath("Library")) {
        QVERIFY(library.open());
        BookMeta meta;
        meta.title = "Saga";
        book = library.addEntry(meta, true).id;
        library.setProgress(book, {9, 1.0, 100});
        Graph graph(library);
        realm = Chronology::timeline(graph, Chronology::Whose::World, "The Realm");
        first = event(graph, "The founding", realm, 1);
        second = event(graph, "The schism", realm, 2);
        third = event(graph, "The reunion", realm, 3);
        const auto otherwise = Chronology::branch(
            graph, {.timeline = 0, .name = "The Realm divided", .from = realm, .at = second, .revealed = std::nullopt, .proposed = false});
        QVERIFY2(otherwise.has_value(), qPrintable(Fixtures::whyNot(otherwise)));
        divided = *otherwise;
        exile = event(graph, "The exile", divided, 4);
        const auto again = Chronology::branch(
            graph, {.timeline = 0, .name = {}, .from = divided, .at = exile, .revealed = std::nullopt, .proposed = false});
        QVERIFY2(again.has_value(), qPrintable(Fixtures::whyNot(again)));
        returned = *again;
        homecoming = event(graph, "The homecoming", returned, 5);
    }
    qint64 event(Graph &graph, const QString &name, qint64 timeline, int year) {
        const qint64 id = graph.upsert({}, "event", name);
        [&] {
            QVERIFY(Chronology::setWhen(graph, id, timeOf(QString(R"({"calendar": "Realm", "at": "%1"})").arg(year))));
            QVERIFY(Chronology::placeOn(graph, id, timeline));
        }();
        return id;
    }

    QTemporaryDir dir;
    Library library;
    qint64 book = 0, realm = 0, first = 0, second = 0, third = 0, divided = 0, exile = 0, returned = 0, homecoming = 0;
};

[[nodiscard]] SpaceSnapshot::Owner ada() {
    static const auto person = Person::generate();
    if (!person)
        qFatal("%s", qPrintable(person.error()));
    return {.name = "Ada Fixture", .person = person->id()};
}

[[nodiscard]] bool tenChapters(const QString &path) {
    QList<Fixtures::Chapter> chapters;
    for (int at = 1; at <= 10; ++at)
        chapters << Fixtures::Chapter{QString("Chapter %1").arg(at), QString("<p>The fork in the road, the %1th day.</p>").arg(at)};
    return Fixtures::chapteredEpub(path, "The Fork", chapters);
}

// A story of a book with a file: the road, the fork in it and the return
// on its own timeline; a branch of it at the fork, revealed in chapter 4,
// with the lost years on it, met in chapter 6.
struct Fork {
    explicit Fork(Library &library, qint64 book) : graph(library) {
        const auto at = [book](int section) { return GraphMoment{{book, section, 0.5}, QString("in chapter %1").arg(section)}; };
        const auto event = [&](const QString &name, int section, int year) {
            const qint64 id = graph.upsert({}, "event", name, {}, {}, GraphPlace{book, section, 0.5}, name);
            std::ignore = Chronology::setWhen(graph, id, timeOf(QString(R"({"calendar": "Road", "at": "%1"})").arg(year)), at(section));
            return id;
        };
        line = Chronology::timelineFor(graph, library, book, Chronology::Whose::Book, {}, at(0));
        road = event("The road", 0, 1);
        fork = event("The fork", 1, 2);
        back = event("The return", 2, 3);
        lost = event("The lost years", 6, 4);
        for (const qint64 placed : {road, fork, back})
            std::ignore = Chronology::placeOn(graph, placed, line, at(0));
        const auto made = Chronology::branch(
            graph, {.timeline = 0, .name = "The other road", .from = line, .at = fork, .revealed = at(4), .proposed = true});
        branch = made.value_or(0);
        std::ignore = Chronology::placeOn(graph, lost, branch, at(6));
    }

    Graph graph;
    qint64 line = 0, road = 0, fork = 0, back = 0, lost = 0, branch = 0;
};

// A stand-in for Storyteller: a story and its branch are books of its own
// (jottings), the story's events on its own timeline, and the branch's
// timeline branching off it at the storm.
const QByteArray manifest = R"({"name": "Teller", "version": "1.0", "script": "main.js", "permissions": ["files"]})";
const QByteArray script = R"js(
omabook.format.register({
    name: "Jotting",
    extensions: ["jot"],
    open: function (source) { return {title: "A story", chapters: [{title: "One", text: source.text(source.entries()[0])}]}; }
});
function write(title) {
    omabook.files.write("data/" + title + ".jot", "Once upon a time, " + title + ".");
    return omabook.library.create({title: title}, "data/" + title + ".jot").id;
}
function event(name, book, section) { return omabook.graph.upsert({kind: "event", name: name, book: book, section: section, fraction: 0.5}); }
function books() { return {story: write("The Story"), fork: write("The Story (branch)")}; }
function tell(story, fork) {
    const line = omabook.graph.timeline(story);
    const meeting = event("The meeting", story, 0), storm = event("The storm", story, 1), wedding = event("The wedding", story, 2);
    [meeting, storm, wedding].forEach(function (one) { omabook.graph.link(one, line, "on", {book: story, section: 0}); });
    omabook.graph.link(meeting, storm, "before", {book: story, section: 1});
    omabook.graph.link(storm, wedding, "before", {book: story, section: 2});
    const branch = omabook.graph.branch({timeline: omabook.graph.timeline(fork), from: line, at: storm, book: story, section: 1});
    const flight = event("The flight", fork, 1);
    omabook.graph.link(flight, branch, "on", {book: fork, section: 1});
    omabook.graph.link(storm, flight, "before", {book: fork, section: 1});
    return {story: story, fork: fork, line: line, branch: branch, storm: storm};
}
function again(line, storm) { return omabook.graph.branch({timeline: line, from: line, at: storm}); }
function timelineOf(book) { return omabook.graph.timeline(book); }
)js";

} // namespace

class ChronologyBranchTests : public QObject {
    Q_OBJECT

private slots:
    // The AI reads that a book departs from real history: it proposes the
    // branch, and puts none of what happens otherwise on real history, not
    // even what it dates on the real calendar.
    void anAlternateHistoryStaysOffRealHistoryAndIsOnlyProposed() {
        const Shelf shelf;
        const Chronicle told = chronicleOf(shelf.library);
        QCOMPARE(told.proposals.size(), 1);
        const Chronology::Proposal &proposal = told.proposals.first();
        QCOMPARE(proposal.timeline.name, branchName);
        QCOMPARE(proposal.split.at.name, QString("The siege of Moscow"));
        QCOMPARE(proposal.split.parent.data.value("timeline").toString(), QString("history"));
        QCOMPARE(proposal.split.revealed.place.bookId, shelf.otherwise);
        QCOMPARE(proposal.split.revealed.place.section, 1);
        const Lane *history = laneNamed(told, proposal.split.parent.name);
        QCOMPARE(names(history), QStringList({"The invasion", "The siege of Moscow", "The armistice"}));
        QVERIFY(!laneNamed(told, branchName));
        // What happens otherwise is in its book's own order, on no timeline it shares with another.
        QVERIFY(onAnyLane(told, "The victory parade") && onAnyLane(told, "The new order"));
        const auto order =
            Chronology::of(Graph(shelf.library), shelf.named("event", "The new order"), GraphContext::readerKnows(shelf.library));
        QVERIFY(order.has_value());
        QVERIFY(std::ranges::none_of(order->timelines, [](const Chronology::Placed &placed) {
            return placed.timeline.data.value("timeline").toString() == "history";
        }));
        QVERIFY(QJsonDocument(told.toJson()).toJson().contains(branchName.toUtf8()));
    }

    // Taken, the branch is a lane right after real history's, which it shares
    // the events up to the split with; what happens otherwise is on it alone.
    void anAcceptedBranchSharesWhatCameBeforeTheSplit() {
        Shelf shelf;
        Graph graph(shelf.library);
        const auto word = Chronology::decide(graph, shelf.branch(), true);
        QVERIFY2(word.has_value(), qPrintable(Fixtures::whyNot(word)));
        const Chronicle told = chronicleOf(shelf.library);
        QVERIFY(told.proposals.isEmpty());
        const Lane *branch = laneNamed(told, branchName);
        QVERIFY(branch && branch->split);
        QCOMPARE(names(branch), QStringList({"The invasion", "The siege of Moscow", "The victory parade"}));
        QCOMPARE(sharedOf(branch), QStringList({"The invasion", "The siege of Moscow"}));
        const QString history = branch->split->parent.name;
        QCOMPARE(names(laneNamed(told, history)), QStringList({"The invasion", "The siege of Moscow", "The armistice"}));
        QCOMPARE(laneAt(told, branchName), laneAt(told, history) + 1);
        // One axis: the branch is drawn beside the timeline it branches off.
        QVERIFY(std::ranges::any_of(told.axes, [&](const Chronology::Axis &axis) {
            return axis.lanes.contains(laneAt(told, branchName)) && axis.lanes.contains(laneAt(told, history));
        }));
        const QJsonObject lane = branch->toJson();
        QCOMPARE(lane.value("branch").toObject().value("at_name").toString(), QString("The siege of Moscow"));
        QVERIFY(lane.value("items").toArray().first().toObject().value("shared").toBool());

        // Building the book again proposes nothing anew.
        QVERIFY(GraphBuild::apply(shelf.library, shelf.otherwise, passagesOf(otherwiseChapters), otherwiseAnswer).understood);
        QVERIFY(chronicleOf(shelf.library).proposals.isEmpty());
        QVERIFY(laneNamed(chronicleOf(shelf.library), branchName));
    }

    void aRefusedBranchIsNoTimelineAndIsNotAskedAgain() {
        Shelf shelf;
        Graph graph(shelf.library);
        QVERIFY(Chronology::decide(graph, shelf.branch(), true));
        QVERIFY(Chronology::decide(graph, shelf.branch(), false));
        const Chronicle told = chronicleOf(shelf.library);
        QVERIFY(told.proposals.isEmpty() && !laneNamed(told, branchName));
        QVERIFY(onAnyLane(told, "The victory parade"));
        QCOMPARE(names(laneNamed(told, "Real history")), QStringList({"The invasion", "The siege of Moscow", "The armistice"}));
        QVERIFY(GraphBuild::apply(shelf.library, shelf.otherwise, passagesOf(otherwiseChapters), otherwiseAnswer).understood);
        QVERIFY(chronicleOf(shelf.library).proposals.isEmpty());
        // Only a branch takes a word.
        QVERIFY(!Chronology::decide(graph, shelf.named("timeline", "Real history"), true));
    }

    // A reader before the split is told nothing of it: not proposed, not drawn.
    void aSplitShowsOnlyOnceItIsRead() {
        const Shelf shelf(0);
        Chronicle told = chronicleOf(shelf.library);
        QVERIFY(told.proposals.isEmpty());
        QVERIFY(!onAnyLane(told, "The victory parade"));
        QVERIFY(!QJsonDocument(told.toJson()).toJson().contains("Alternate history"));

        QTemporaryDir dir;
        QVERIFY(tenChapters(dir.filePath("fork.epub")));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const qint64 book = library.add(dir.filePath("fork.epub")).id;
        Fork fork(library, book);
        QVERIFY(fork.branch > 0);
        QVERIFY(Chronology::decide(fork.graph, fork.branch, true));
        library.setProgress(book, {3, 0.9, 30});
        told = chronicleOf(library);
        QVERIFY(!laneNamed(told, "The other road") && told.proposals.isEmpty());
        library.setProgress(book, {5, 0.9, 50});
        told = chronicleOf(library);
        QCOMPARE(names(laneNamed(told, "The other road")), QStringList({"The road", "The fork"}));
        library.setProgress(book, {9, 1.0, 100});
        QCOMPARE(names(laneNamed(chronicleOf(library), "The other road")), QStringList({"The road", "The fork", "The lost years"}));
    }

    void aBranchBranchesAgainAndNeverOffItself() {
        Realm realm;
        const Chronicle told = chronicleOf(realm.library);
        QCOMPARE(names(laneNamed(told, "The Realm")), QStringList({"The founding", "The schism", "The reunion"}));
        QCOMPARE(names(laneNamed(told, "The Realm divided")), QStringList({"The founding", "The schism", "The exile"}));
        const QString again = "The Realm divided from The exile";
        QCOMPARE(names(laneNamed(told, again)), QStringList({"The founding", "The schism", "The exile", "The homecoming"}));
        QCOMPARE(sharedOf(laneNamed(told, again)), QStringList({"The founding", "The schism", "The exile"}));
        QCOMPARE(laneAt(told, "The Realm divided"), laneAt(told, "The Realm") + 1);
        QCOMPARE(laneAt(told, again), laneAt(told, "The Realm") + 2);

        Graph graph(realm.library);
        const auto circle = Chronology::branch(
            graph,
            {.timeline = realm.realm, .name = {}, .from = realm.returned, .at = realm.exile, .revealed = std::nullopt, .proposed = false});
        QVERIFY(!circle);
        QVERIFY(circle.error().text().contains("branch off itself"));
        QVERIFY(!Chronology::branch(graph, {.timeline = realm.divided,
                                            .name = {},
                                            .from = realm.divided,
                                            .at = realm.first,
                                            .revealed = std::nullopt,
                                            .proposed = false}));
        QVERIFY(!Chronology::branch(
            graph,
            {.timeline = 0, .name = "Elsewhere", .from = realm.first, .at = realm.first, .revealed = std::nullopt, .proposed = false}));
        QVERIFY(!Chronology::branch(
            graph,
            {.timeline = 0, .name = "The Realm", .from = realm.realm, .at = realm.first, .revealed = std::nullopt, .proposed = false}));
        // Branched anew, it takes the new split and leaves the old.
        QVERIFY(Chronology::branch(
            graph,
            {.timeline = realm.divided, .name = {}, .from = realm.realm, .at = realm.first, .revealed = std::nullopt, .proposed = false}));
        QCOMPARE(names(laneNamed(chronicleOf(realm.library), "The Realm divided")), QStringList({"The founding", "The exile"}));
        QCOMPARE(Chronology::parentOf(graph, realm.divided), realm.realm);
    }

    // Drawn, the branch begins where it splits off its parent's lane, and
    // what it shares is marked on that lane alone.
    void aBranchIsDrawnBesideItsParentFromTheSplitOn() {
        const Realm realm;
        const Chronology::Chart::Chart chart = Chronology::Chart::chart({{.name = {}, .chronicle = chronicleOf(realm.library)}});
        const auto laneOf = [&chart](const QString &name) {
            return std::ranges::find(chart.lanes, name, [](const Chronology::Chart::LaneOf &lane) { return lane.timeline.name; })
                   - chart.lanes.cbegin();
        };
        const auto divided = laneOf("The Realm divided");
        QVERIFY(divided < chart.lanes.size());
        QVERIFY(chart.lanes.at(divided).split.has_value());
        QCOMPARE(chart.lanes.at(divided).parent, static_cast<int>(laneOf("The Realm")));
        QCOMPARE(chart.lanes.at(laneOf("The Realm divided from The exile")).parent, static_cast<int>(divided));
        QCOMPARE(chart.marksOf.value(realm.first).size(), 1);
        QCOMPARE(chart.marksOf.value(realm.second).size(), 1);
        const Chronology::Chart::Mark &schism = chart.marks.at(chart.marksOf.value(realm.second).first());
        QCOMPARE(schism.lane, static_cast<int>(laneOf("The Realm")));
        QCOMPARE(chart.lanes.at(divided).splitAt, schism.from);
        QCOMPARE(chart.marks.at(chart.marksOf.value(realm.exile).first()).lane, static_cast<int>(divided));
        QVERIFY(chart.marks.at(chart.marksOf.value(realm.exile).first()).from > schism.from);
    }

    // The window lists what the AI proposed, and the reader's word takes it
    // or not, as far as they read.
    void theWindowAsksAndTakesTheReadersWord() {
        Shelf shelf;
        ChronologyBrowser browser;
        browser.setLibrary(&shelf.library);
        browser.showScope();
        ChronologyProposals proposals;
        proposals.setLibrary(&shelf.library);
        proposals.setBrowser(&browser);
        QCOMPARE(proposals.list().size(), 1);
        const QVariantMap proposal = proposals.list().first().toMap();
        QCOMPARE(proposal.value("name").toString(), branchName);
        QCOMPARE(proposal.value("at").toString(), QString("The siege of Moscow"));
        const int lanes = browser.laneCount();
        QVERIFY(proposals.decide(proposal.value("timeline").toLongLong(), true));
        QVERIFY(proposals.list().isEmpty());
        // What happened otherwise leaves its book's own order for the branch, which is beside real history.
        QVERIFY(browser.laneCount() < lanes);
        QStringList laneNames;
        for (int lane = 0; lane < browser.laneCount(); ++lane)
            laneNames << browser.laneName(lane);
        QVERIFY2(laneNames.contains(branchName + ", off Real history at The siege of Moscow"), qPrintable(laneNames.join('\n')));
        QVERIFY(!proposals.decide(shelf.named("timeline", "Real history"), false));
    }

    // Storyteller's branch: a story's branch is a branch of the story's own
    // timeline, made through the plugin API.
    void aStorysBranchIsABranchOfItsTimeline() {
        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const QString plugins = dir.filePath("plugins");
        QVERIFY(QDir().mkpath(plugins + "/teller"));
        QVERIFY(Fixtures::write(plugins + "/teller/plugin.json", manifest));
        QVERIFY(Fixtures::write(plugins + "/teller/main.js", script));
        ScriptServices services;
        services.library = [&library] { return &library; };
        ScriptPlugins set(services, plugins, nullptr, false);
        set.provideFormats();
        ScriptPlugin *teller = set.plugin("teller");
        QVERIFY(teller);
        QString problem;
        const QJsonObject made = teller->callNow("books", {}, &problem).toObject();
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        // The window has a new book's tenants follow it; here the test does.
        Tenancy::follow(library);
        const QJsonObject told = teller->callNow("tell", {made.value("story"), made.value("fork")}, &problem).toObject();
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        const qint64 story = told.value("story").toInteger(), fork = told.value("fork").toInteger();
        QVERIFY(story > 0 && fork > 0 && told.value("branch").toInteger() > 0);
        QCOMPARE(teller->callNow("timelineOf", {story}, &problem).toInteger(), told.value("line").toInteger());
        library.setProgress(story, {0, 1.0, 100});
        library.setProgress(fork, {0, 1.0, 100});

        const Chronicle chronicle = chronicleOf(library);
        QCOMPARE(names(laneNamed(chronicle, "The Story")), QStringList({"The meeting", "The storm", "The wedding"}));
        const Lane *branch = laneNamed(chronicle, "The Story (branch)");
        QVERIFY(branch && branch->split);
        QCOMPARE(branch->split->parent.name, QString("The Story"));
        QCOMPARE(names(branch), QStringList({"The meeting", "The storm", "The flight"}));
        QCOMPARE(sharedOf(branch), QStringList({"The meeting", "The storm"}));

        teller->callNow("again", {told.value("line"), told.value("storm")}, &problem);
        QVERIFY2(problem.contains("branch off itself"), qPrintable(problem));
        teller->callNow("timelineOf", {9999}, &problem);
        QVERIFY2(problem.contains("no book 9999"), qPrintable(problem));
    }

    // A branch, its split and the reader's word go to one's other computer through the log.
    void aBranchTravelsThroughTheLog() {
        QTemporaryDir dir;
        QVERIFY(tenChapters(dir.filePath("fork.epub")));
        Library here(dir.filePath("Here")), there(dir.filePath("There"));
        QVERIFY(here.open() && there.open());
        const qint64 mine = here.add(dir.filePath("fork.epub")).id;
        const qint64 theirs = there.add(dir.filePath("fork.epub")).id;
        here.setProgress(mine, {9, 1.0, 100});
        there.setProgress(theirs, {9, 1.0, 100});
        Fork fork(here, mine);
        // Another the AI proposed, not answered yet.
        QVERIFY(Chronology::branch(fork.graph, {.timeline = 0,
                                                .name = "The road not taken",
                                                .from = fork.line,
                                                .at = fork.road,
                                                .revealed = GraphMoment{{mine, 2, 0.5}, {}},
                                                .proposed = true}));
        QVERIFY(Chronology::decide(fork.graph, fork.branch, true));
        const Remote remote("laptop:" + dir.filePath("There"), {Fixtures::standInSsh(dir.path(), OMABOOK_BINARY)});
        const SyncSummary summary = syncLibrary(here, remote);
        QVERIFY2(summary.problems.isEmpty(), qPrintable(summary.problems.join('\n')));

        const Chronicle told = chronicleOf(there);
        const Lane *branch = laneNamed(told, "The other road");
        QVERIFY(branch && branch->split);
        QCOMPARE(branch->split->at.name, QString("The fork"));
        QCOMPARE(branch->split->revealed.place.section, 4);
        QCOMPARE(names(branch), QStringList({"The road", "The fork", "The lost years"}));
        QCOMPARE(told.proposals.size(), 1);
        QCOMPARE(told.proposals.first().timeline.name, QString("The road not taken"));
        QCOMPARE(syncLibrary(here, remote).graphTaken, 0);
    }

    // In a space someone subscribes to, a branch is there as far as they read their copy.
    void aBranchGoesIntoASpaceCutAtTheReadersPlace() {
        QTemporaryDir dir;
        QVERIFY(tenChapters(dir.filePath("fork.epub")));
        Library owner(dir.filePath("Owner"));
        QVERIFY(owner.open());
        const qint64 book = owner.add(dir.filePath("fork.epub")).id;
        owner.setProgress(book, {9, 1.0, 100});
        const auto shelf = owner.addCollection("Roads");
        QVERIFY(shelf.has_value() && owner.putInCollection(*shelf, book, true));
        Fork fork(owner, book);
        QVERIFY(Chronology::decide(fork.graph, fork.branch, true));
        const auto snapshot = PublishedSpace::snapshot(owner, *shelf, {.graph = true}, ada());
        QVERIFY2(snapshot.has_value(), qPrintable(Fixtures::whyNot(snapshot)));

        Library receiver(dir.filePath("Receiver"));
        QVERIFY(receiver.open());
        QVERIFY(QFile::copy(dir.filePath("fork.epub"), dir.filePath("mine.epub")));
        const qint64 mine = receiver.add(dir.filePath("mine.epub")).id;
        const auto subscribed = Subscriptions::subscribe(receiver, *snapshot, {});
        QVERIFY2(subscribed.has_value(), qPrintable(Fixtures::whyNot(subscribed)));
        const auto layered = [&receiver] {
            return Chronology::chronicle(Chronology::read(GraphContext::layers(Graph::ofLibrary(receiver)).first()));
        };
        receiver.setProgress(mine, {2, 0.9, 25});
        QVERIFY(!laneNamed(layered(), "The other road"));
        receiver.setProgress(mine, {9, 1.0, 100});
        const Chronicle told = layered();
        QCOMPARE(names(laneNamed(told, "The other road")), QStringList({"The road", "The fork", "The lost years"}));
        QVERIFY(told.proposals.isEmpty());
    }
};

QTEST_MAIN(ChronologyBranchTests)
#include "chronologybranch_tests.moc"
