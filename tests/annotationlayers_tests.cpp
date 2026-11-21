// Others' annotations (#85): a space's annotations part, off until switched
// on, carries its members' highlights, margin notes and ink into it, and each
// other member sees them as an annotation layer, cut at their own place in
// the book (never ahead), each person under their name and in their colour.
// What another person sent is untrusted: plain words, within ADR 0002's
// limits. And a margin note becomes a fact of the graph, learned at its place.

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <initializer_list>
#include <vector>

#include "annotationfacts.h"
#include "annotationink.h"
#include "annotationlayers.h"
#include "changelog.h"
#include "fixtures.h"
#include "graph.h"
#include "library.h"
#include "membernames.h"
#include "person.h"
#include "plugindata.h"
#include "publishedspace.h"
#include "sharedspace.h"
#include "sharedspaces.h"
#include "spaceparts.h"
#include "spaceswitch.h"
#include "subscribedspace.h"
#include "subscriptions.h"
#include "untrustedtext.h"

namespace {

using AnnotationLayers::Layers;
using Fixtures::whyNot;

// One person's computer: their library and their own Omabook data folder.
struct Someone {
    Someone(const QTemporaryDir &dir, const QString &name) : library(dir.filePath(name + "/Library")) {
        library.usePersonFile(dir.filePath(name + "/home/omabook/person.key"));
        QVERIFY(library.open());
        QVERIFY(library.person() != nullptr);
        id = library.person()->id();
    }

    // Their copy of the club's book, as their collection of the club holds it.
    [[nodiscard]] qint64 dune() const {
        const auto club = library.collectionNamed("Club");
        if (!club)
            return 0;
        const QList<Book> books = club->of(library.books());
        return books.isEmpty() ? 0 : books.first().id;
    }
    void readTo(int section) { library.setProgress(dune(), {section, 0.5, section * 10.0}); }

    Library library;
    QString id;
};

void synced(Someone &who, const QString &space, const QString &folder) {
    const auto done = SharedSpaces::sync(who.library, space, folder);
    QVERIFY2(done.has_value(), qPrintable(whyNot(done)));
}

// A club founded at `founder`'s with Dune, `other` invited as a writer and joined, both in step.
[[nodiscard]] QString clubOf(Someone &founder, Someone &other, const QTemporaryDir &dir, const QString &folder) {
    QDir().mkpath(folder);
    const auto created = SharedSpaces::create(founder.library, "Club", {});
    if (!created)
        qFatal("%s", qPrintable(created.error()));
    const QString space = created->space.space;
    BookMeta meta;
    meta.title = "Dune";
    meta.authors = {"Frank Herbert"};
    meta.isbn = "9780441172719";
    if (!founder.library.putInCollection(founder.library.collectionNamed("Club")->id, founder.library.addEntry(meta, true).id, true))
        qFatal("no Dune");
    if (const auto invited = SharedSpaces::invite(founder.library, space, other.id, SharedSpace::Role::Writer, dir.filePath("invite"));
        !invited)
        qFatal("%s", qPrintable(invited.error()));
    synced(founder, space, folder);
    if (const auto joined = SharedSpaces::join(other.library, dir.filePath("invite")); !joined)
        qFatal("%s", qPrintable(joined.error()));
    synced(other, space, folder);
    return space;
}

// A margin note or a highlight on Dune, at the start of `section`.
qint64 annotate(Someone &who, const QString &kind, int section, const QString &quote, const QString &note = {}) {
    Annotation annotation{.id = 0, .kind = kind, .place = {}, .text = quote, .note = note, .created = 0};
    annotation.setPassage({.from = {.section = section, .fraction = 0.1},
                           .to = {.section = section, .fraction = 0.2},
                           .quote = quote,
                           .before = {},
                           .after = {}});
    return who.library.addAnnotation(who.dune(), annotation);
}

// What `who` sees of others': each annotation's words, by whose they are.
[[nodiscard]] QStringList seen(const Library &library) {
    QStringList words;
    for (const AnnotationLayers::Person &person : AnnotationLayers::read(library).people)
        for (const AnnotationLayers::Annotation &one : person.annotations)
            words << AnnotationLayers::calledOf(person).left(8) + ": " + (one.note.isEmpty() ? one.quote : one.note);
    words.sort();
    return words;
}

[[nodiscard]] Ink::Stroke stroke(int at) {
    Ink::Stroke drawn{
        .id = Ink::newId(), .color = Qt::blue, .width = 2, .pressured = false, .tilted = false, .began = 1000 + at, .points = {}};
    for (int i = 0; i < 2; ++i)
        drawn.points.push_back({.at = {at + i * 1.0, 10.0 + i}, .pressure = 0.5F, .xTilt = 0, .yTilt = 0, .ms = static_cast<quint32>(i)});
    return drawn;
}

} // namespace

class AnnotationLayersTests : public QObject {
    Q_OBJECT

private slots:
    // The part is in the registry, off by default and switched later.
    void theAnnotationsPartIsRegisteredAndOffByDefault() {
        const SpaceParts::Part *part = SpaceParts::named(u"annotations");
        QVERIFY(part != nullptr);
        QVERIFY(part->switched);
        QVERIFY(!SpaceParts::Parts{}.annotations);
        QVERIFY(SpaceParts::Parts::fromNames({"annotations"}).annotations);
        QCOMPARE((SpaceParts::Parts{.graph = true, .annotations = true}.names()), (QStringList{"graph", "annotations"}));
        QVERIFY(SpaceParts::names().contains("annotations"));
        QVERIFY(SpaceParts::switchedBy(u"part:annotations") == part);
        QVERIFY(SpaceParts::switchedBy(u"part:graph") == nullptr);
    }

    // Two members, one ahead and one behind: nothing goes until the part is
    // on; then each sees the other's as far as they have read themselves,
    // and never their own; switched off, it is all gone again.
    void aClubOfTwoSeesEachOthersAnnotationsCutAtTheirOwnPlace() {
        QTemporaryDir dir;
        Someone ada(dir, "ada"), bo(dir, "bo");
        const QString folder = dir.filePath("Shared");
        const QString space = clubOf(ada, bo, dir, folder);
        QVERIFY(ada.dune() > 0 && bo.dune() > 0);
        ada.readTo(9); // ahead: near the end
        bo.readTo(3);  // behind
        annotate(ada, "margin-note", 2, "the spice must flow", "Ada: the spice again");
        annotate(ada, "highlight", 6, "fear is the mind-killer");
        annotate(ada, "bookmark", 1, "Ada's place");
        annotate(bo, "margin-note", 1, "a desert planet", "Bo: Arrakis");
        synced(ada, space, folder);
        synced(bo, space, folder);
        synced(ada, space, folder);
        QVERIFY2(seen(bo.library).isEmpty(), qPrintable(seen(bo.library).join('\n')));
        QVERIFY(seen(ada.library).isEmpty());

        const auto on = SpaceSwitch::set(bo.library, space, "annotations", true);
        QVERIFY2(on.has_value(), qPrintable(whyNot(on)));
        QVERIFY(on->changed && on->on);
        QVERIFY(!SpaceSwitch::set(bo.library, space, "annotations", true)->changed);
        QVERIFY(!SpaceSwitch::set(bo.library, space, "graph", true).has_value());
        synced(bo, space, folder);
        synced(ada, space, folder);
        synced(bo, space, folder);
        QVERIFY(SpaceSwitch::isOn(ada.library, space, "annotations"));

        // Bo, behind, sees Ada's note of chapter 2 and not her highlight of
        // chapter 6, nor her bookmark; Ada, ahead, sees Bo's note.
        const Layers behind = AnnotationLayers::read(bo.library);
        QCOMPARE(behind.people.size(), 1);
        QCOMPARE(behind.people.first().person, ada.id);
        QCOMPARE(behind.people.first().annotations.size(), 1);
        QCOMPARE(behind.people.first().annotations.first().note, QString("Ada: the spice again"));
        QCOMPARE(behind.people.first().annotations.first().book, bo.dune());
        QCOMPARE(seen(ada.library).size(), 1);
        QVERIFY(seen(ada.library).first().endsWith("Bo: Arrakis"));
        // The same colour for Ada on every computer, and another for Bo.
        QCOMPARE(AnnotationLayers::toJson(behind.people.first()).value("color").toString(), AnnotationLayers::colorOf(ada.id).name());
        QVERIFY(AnnotationLayers::colorOf(ada.id) != AnnotationLayers::colorOf(bo.id));

        // Reading on brings the rest; a place asked for further on never does.
        bo.readTo(7);
        QCOMPARE(AnnotationLayers::read(bo.library).people.first().annotations.size(), 2);
        const GraphKnown early = [](const GraphPlace &at) { return at.section < 3; };
        QCOMPARE(AnnotationLayers::read(bo.library, early).people.first().annotations.size(), 1);

        // Ada changes her note and removes her highlight: both reach Bo.
        Annotation note = ada.library.annotations(ada.dune(), "margin-note").first();
        note.note = "Ada: the spice, again";
        QVERIFY(ada.library.storeAnnotation(ada.dune(), note));
        ada.library.removeAnnotation(ada.library.annotations(ada.dune(), "highlight").first().id);
        synced(ada, space, folder);
        synced(bo, space, folder);
        QCOMPARE(seen(bo.library).size(), 1);
        QVERIFY(seen(bo.library).first().endsWith("Ada: the spice, again"));

        // Off: nothing of anyone's is shown any more.
        QVERIFY(SpaceSwitch::set(ada.library, space, "annotations", false).has_value());
        synced(ada, space, folder);
        synced(bo, space, folder);
        QVERIFY(seen(bo.library).isEmpty());
        QVERIFY(seen(ada.library).isEmpty());
    }

    // Two members, each going by the name they chose (#124): it travels in the
    // space as their own operation, and is shown with their annotations. A
    // name is untrusted: plain, on one line, no longer than 64 characters; a
    // name one member gives another, or one too long, is passed over.
    void membersGoByTheNamesTheyChose() {
        QTemporaryDir dir;
        Someone ada(dir, "ada"), bo(dir, "bo");
        const QString folder = dir.filePath("Shared");
        const QString space = clubOf(ada, bo, dir, folder);
        ada.readTo(9);
        bo.readTo(9);
        QVERIFY(SpaceSwitch::set(ada.library, space, "annotations", true).has_value());
        annotate(ada, "margin-note", 2, "the spice must flow", "Ada's note");
        annotate(bo, "margin-note", 1, "a desert planet", "Bo's note");
        // Before either chose one, each is the start of their id.
        synced(ada, space, folder);
        synced(bo, space, folder);
        synced(ada, space, folder);
        QCOMPARE(AnnotationLayers::calledOf(AnnotationLayers::read(ada.library).people.first()), bo.id.left(8));

        const auto named = SharedSpaces::goBy(bo.library, space, QString("Bo\u202E\n  Ek\x07"));
        QVERIFY2(named.has_value(), qPrintable(whyNot(named)));
        QVERIFY(SharedSpaces::goBy(ada.library, "Club", "Ada Lovelace").has_value());
        const auto tooLong = SharedSpaces::goBy(bo.library, space, QString(UntrustedText::maxNameLength + 1, 'x'));
        QVERIFY(!tooLong.has_value());
        QCOMPARE(tooLong.error().code(), Failure::Code::NotAName);
        QVERIFY(!SharedSpaces::goBy(bo.library, space, " \t ").has_value());
        synced(bo, space, folder);
        synced(ada, space, folder);
        synced(bo, space, folder);

        const Layers atAda = AnnotationLayers::read(ada.library);
        QCOMPARE(atAda.people.size(), 1);
        QCOMPARE(atAda.people.first().name, QString("Bo Ek"));
        QCOMPARE(AnnotationLayers::toJson(atAda.people.first()).value("name").toString(), QString("Bo Ek"));
        QCOMPARE(AnnotationLayers::read(bo.library).people.first().name, QString("Ada Lovelace"));
        // The members as the space lists them: each by their name, oneself as you.
        const QList<SharedSpaces::Summary> spaces = SharedSpaces::list(ada.library);
        const auto club = std::ranges::find(spaces, space, &SharedSpaces::Summary::space);
        QVERIFY(club != spaces.cend());
        QStringList members;
        for (const SharedSpaces::Member &member : club->members)
            members << member.name;
        members.sort();
        QCOMPARE(members, (QStringList{"Bo Ek", "you, Ada Lovelace"}));

        // Ada names Bo, and Bo writes a name too long by hand: neither counts.
        const QList<ChangeLog::Entry> forged{MemberNames::entry(space, bo.id, "Mallory")};
        QVERIFY(SharedSpace::write(ada.library, space, forged).has_value());
        const QList<ChangeLog::Entry> byHand{MemberNames::entry(space, bo.id, QString(500, 'y'))};
        QVERIFY(SharedSpace::write(bo.library, space, byHand).has_value());
        synced(ada, space, folder);
        synced(bo, space, folder);
        synced(ada, space, folder);
        QCOMPARE(AnnotationLayers::read(ada.library).people.first().name, QString("Bo Ek"));
        QCOMPARE(MemberNames::of(SharedSpace::state(ada.library, space).accepted).value(bo.id), QString("Bo Ek"));
    }

    // A published space with the part carries its owner's annotations to whoever
    // subscribes, cut at the subscriber's place; one removed is taken back.
    void aPublishedSpaceCarriesItsOwnersAnnotations() {
        QTemporaryDir dir;
        Library owner(dir.filePath("Owner"));
        QVERIFY(owner.open());
        QVERIFY(Fixtures::epub(dir.filePath("book.epub"), "The Long Tide"));
        const qint64 book = owner.add(dir.filePath("book.epub")).id;
        const auto shelf = owner.addCollection("Tides");
        QVERIFY(shelf && owner.putInCollection(*shelf, book, true));
        Annotation note{.id = 0, .kind = "margin-note", .place = {}, .text = "ferry", .note = "Owner's note", .created = 0};
        note.setPassage(
            {.from = {.section = 1, .fraction = 0.1}, .to = {.section = 1, .fraction = 0.2}, .quote = "ferry", .before = {}, .after = {}});
        const qint64 made = owner.addAnnotation(book, note);
        const auto person = Person::generate();
        QVERIFY(person.has_value());
        const SpaceSnapshot::Owner as{.name = "Ann Owner", .person = person->id()};

        Library receiver(dir.filePath("Receiver"));
        QVERIFY(receiver.open());
        const qint64 mine = receiver.add(dir.filePath("book.epub")).id;
        // Without the part, nothing of it.
        auto without = PublishedSpace::snapshot(owner, *shelf, {}, as);
        QVERIFY(without.has_value());
        QVERIFY(std::ranges::none_of(without->operations, [](const SpaceOperation &op) { return op.entity == "annotation"; }));
        const auto with = PublishedSpace::snapshot(owner, *shelf, {.annotations = true}, as);
        QVERIFY(with.has_value());
        QVERIFY(with->parts.contains("annotations"));
        QVERIFY(Subscriptions::subscribe(receiver, *with, {}).has_value());
        QVERIFY(AnnotationLayers::read(receiver).people.isEmpty()); // not begun
        receiver.setProgress(mine, {2, 0.0, 20});
        const Layers layers = AnnotationLayers::read(receiver);
        QCOMPARE(layers.people.size(), 1);
        QCOMPARE(layers.people.first().name, QString("Ann Owner"));
        QCOMPARE(layers.people.first().annotations.first().note, QString("Owner's note"));

        owner.removeAnnotation(made);
        const auto after = PublishedSpace::snapshot(owner, *shelf, {.annotations = true}, as);
        QVERIFY(after.has_value());
        QVERIFY(Subscriptions::subscribe(receiver, *after, {}).has_value());
        QVERIFY(AnnotationLayers::read(receiver).people.isEmpty());
    }

    // Another member's words are plain text, and what is over the limits is
    // refused: a value over 64 KiB, a change of more than 1000 strokes, and of
    // one annotation no more than 1000 strokes are drawn.
    void othersWordsArePlainAndWithinTheLimits() {
        QTemporaryDir dir;
        Someone ada(dir, "ada"), bo(dir, "bo");
        const QString folder = dir.filePath("Shared");
        const QString space = clubOf(ada, bo, dir, folder);
        ada.readTo(9);
        QVERIFY(SpaceSwitch::set(bo.library, space, "annotations", true).has_value());
        // A handwritten note of Bo's with more strokes than are drawn.
        const qint64 inked = annotate(bo, "margin-note", 1, "a desert planet", "Inked");
        std::vector<Ink::Stroke> many;
        for (int i = 0; i < PluginData::maxItems + 100; ++i)
            many.push_back(stroke(i));
        QVERIFY(AnnotationInk::write(bo.library, inked, {.added = many, .removed = {}}));
        synced(bo, space, folder);
        // Then what Bo's Omabook would never write, written by hand into the space.
        const QString gid = "annotation:by-hand";
        const QString book = SubscribedSpace::bookIn(bo.library, space, bo.dune());
        QVERIFY(!book.isEmpty());
        QJsonObject tooMany;
        for (int i = 0; i <= PluginData::maxItems; ++i)
            tooMany.insert(QString("s%1").arg(i), "x");
        const QList<ChangeLog::Entry> written{
            {.space = space, .entity = "annotation", .id = gid, .field = "book", .value = book, .hlc = {}},
            {.space = space, .entity = "annotation", .id = gid, .field = "kind", .value = "margin-note", .hlc = {}},
            {.space = space, .entity = "annotation", .id = gid, .field = "section", .value = "2", .hlc = {}},
            {.space = space, .entity = "annotation", .id = gid, .field = "quote", .value = "spice", .hlc = {}},
            {.space = space,
             .entity = "annotation",
             .id = gid,
             .field = "note",
             .value = QString("<b>bold</b> <img src=x onerror=alert(1)>\u202Eevil\x07\nline two"),
             .hlc = {}},
            {.space = space, .entity = "annotation", .id = gid, .field = "note", .value = QString(70 * 1024, 'a'), .hlc = {}},
            {.space = space,
             .entity = "annotation",
             .id = gid,
             .field = "set:strokes",
             .value = QString::fromUtf8(QJsonDocument(QJsonObject{{"add", tooMany}}).toJson(QJsonDocument::Compact)),
             .hlc = {}}};
        QVERIFY(SharedSpace::write(bo.library, space, written).has_value());
        synced(bo, space, folder);
        synced(ada, space, folder);

        const Layers layers = AnnotationLayers::read(ada.library);
        QCOMPARE(layers.people.size(), 1);
        QCOMPARE(layers.refused, 2); // the 70 KiB note and the change of 1001 strokes
        const QList<AnnotationLayers::Annotation> &theirs = layers.people.first().annotations;
        const auto byHand = std::ranges::find(theirs, gid, &AnnotationLayers::Annotation::id);
        QVERIFY(byHand != theirs.end());
        // Markup stays words, shown as such; what turns text round or rings a bell is gone.
        QCOMPARE(byHand->note, QString("<b>bold</b> <img src=x onerror=alert(1)>evil\nline two"));
        QVERIFY(byHand->strokes.empty());
        const auto ink = std::ranges::find(theirs, QString("Inked"), &AnnotationLayers::Annotation::note);
        QVERIFY(ink != theirs.end());
        QCOMPARE(static_cast<qsizetype>(ink->strokes.size()), PluginData::maxItems);
        QCOMPARE(UntrustedText::plain(QString("a\u200Fb\u2066c\td") + QChar(0) + "e"), QString("abc\tde"));
    }

    // A margin note, one's own or someone else's, becomes a fact of a thing of
    // the book's graph, learned at its place: not known before it.
    void aNoteBecomesAFactLearnedAtItsPlace() {
        QTemporaryDir dir;
        Someone ada(dir, "ada"), bo(dir, "bo");
        const QString folder = dir.filePath("Shared");
        const QString space = clubOf(ada, bo, dir, folder);
        ada.readTo(9);
        bo.readTo(5);
        QVERIFY(SpaceSwitch::set(ada.library, space, "annotations", true).has_value());
        annotate(ada, "margin-note", 3, "the sleeper must awaken", "Paul is the Kwisatz Haderach");
        synced(ada, space, folder);
        synced(bo, space, folder);
        const Layers layers = AnnotationLayers::read(bo.library);
        QCOMPARE(layers.people.size(), 1);
        const QString theirs = layers.people.first().annotations.first().id;

        // Of a thing that isn't there, without a kind: refused; with one, made.
        const auto source = AnnotationFacts::of(bo.library, theirs);
        QVERIFY2(source.has_value(), qPrintable(whyNot(source)));
        QCOMPARE(source->book, bo.dune());
        QCOMPARE(source->place.section, 3);
        QCOMPARE(AnnotationFacts::make(bo.library, *source, {.which = "Paul", .kind = {}}).error().code(), Failure::Code::NoSuchThing);
        const auto made = AnnotationFacts::make(bo.library, *source, {.which = "Paul", .kind = "character"});
        QVERIFY2(made.has_value(), qPrintable(whyNot(made)));
        QVERIFY(made->madeNode && made->fact > 0);
        const Graph graph = Graph::ofReader(bo.library, bo.dune());
        const std::optional<GraphNode> paul = graph.node(made->node);
        QVERIFY(paul.has_value());
        QVERIFY(!graph.asOf(*paul, bo.dune(), GraphPlace{bo.dune(), 2, 0.5}).summary.contains("Kwisatz"));
        QVERIFY(graph.asOf(*paul, bo.dune(), GraphPlace{bo.dune(), 4, 0.0}).summary.contains("Kwisatz"));
        const QList<GraphFact> facts = graph.facts(made->node);
        QVERIFY(std::ranges::any_of(facts, [&](const GraphFact &fact) {
            return fact.value == "Paul is the Kwisatz Haderach" && fact.revealed.place.bookId == bo.dune()
                   && fact.revealed.place.section == 3;
        }));

        // One's own note does too, of a thing there is, by its name.
        const qint64 own = annotate(bo, "margin-note", 4, "a desert planet", "Arrakis is Dune");
        const auto mine = AnnotationFacts::of(bo.library, QString::number(own));
        QVERIFY(mine.has_value());
        const auto kept = AnnotationFacts::make(bo.library, *mine, {.which = "paul", .kind = {}});
        QVERIFY(kept.has_value());
        QCOMPARE(kept->node, made->node);
        QVERIFY(!kept->madeNode);
        QCOMPARE(AnnotationFacts::of(bo.library, "nothing-of-that-id").error().code(), Failure::Code::NoSuchAnnotation);
    }
};

QTEST_GUILESS_MAIN(AnnotationLayersTests)
#include "annotationlayers_tests.moc"
