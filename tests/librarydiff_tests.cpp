// The library diff (#183): what changed between two copies of one library,
// told as the person sees it, by global id; and one book put back as the
// earlier copy had it, its details or its annotations, losing nothing made
// since. Two libraries of the test's own, the second a copy of the first;
// a GUI application, as the PDF a book is drawn on is made with one.

#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

#include "annotationanchors.h"
#include "bookink.h"
#include "bookmeta.h"
#include "bookrestore.h"
#include "fixtures.h"
#include "graph.h"
#include "inkstroke.h"
#include "library.h"
#include "librarydiff.h"
#include "spaces.h"

using Fixtures::whyNot;

namespace {

[[nodiscard]] qint64 entry(Library &library, const QString &title, const QString &author, const QString &isbn = {}) {
    BookMeta meta;
    meta.title = title;
    meta.authors = {author};
    meta.isbn = isbn;
    return library.addEntry(meta, true).id;
}

[[nodiscard]] Annotation highlight(const QString &text, int section) {
    Annotation made;
    made.kind = "highlight";
    made.place = {.section = section, .fraction = 0.25};
    made.text = text;
    return made;
}

[[nodiscard]] Ink::Stroke stroke(const QString &id, qint64 began) {
    Ink::Stroke made{.id = id, .color = QColor("#1a1a1a"), .width = 2.4, .pressured = false, .tilted = false, .began = began, .points = {}};
    for (int i = 0; i <= 4; ++i)
        made.points.push_back(
            {.at = QPointF(100 + (i * 50), 200), .pressure = 0.5F, .xTilt = 0, .yTilt = 0, .ms = static_cast<quint32>(i * 8)});
    return made;
}

[[nodiscard]] QStringList titles(const QList<LibraryDiff::BookChange> &books) {
    QStringList found;
    for (const LibraryDiff::BookChange &book : books)
        found << book.title;
    found.sort();
    return found;
}

[[nodiscard]] QStringList strokeIds(const Library &library, qint64 book, int page) {
    QStringList ids;
    for (const Ink::Stroke &one : BookInk::strokes(library, book, page))
        ids << one.id;
    ids.sort();
    return ids;
}

[[nodiscard]] QStringList highlightTexts(const Library &library, qint64 book) {
    QStringList texts;
    for (const Annotation &annotation : library.annotations(book, "highlight"))
        texts << annotation.text;
    texts.sort();
    return texts;
}

// A library, and a copy of it as it stands: the snapshot's side, and the side that changes on.
struct Copied {
    QTemporaryDir dir;
    std::unique_ptr<Library> before = std::make_unique<Library>(dir.filePath("Before"));
    std::unique_ptr<Library> after;

    [[nodiscard]] bool copy() {
        Spaces::assignAll(*before);
        if (!QDir().mkpath(dir.filePath("After")) || !before->copyInto(Library::databaseIn(dir.filePath("After"))))
            return false;
        after = std::make_unique<Library>(dir.filePath("After"));
        return after->open();
    }
};

} // namespace

class LibraryDiffTests : public QObject {
    Q_OBJECT

private slots:
    // Books added, removed and changed field by field, matched by global id:
    // a book renamed is changed, not one gone and another come. A book with
    // only a highlight more is changed too, by its notes alone; the graph's
    // new things and facts are counted.
    void aDiffTellsBooksAddedRemovedAndChangedByGlobalId() {
        Copied library;
        QVERIFY(library.before->open());
        Library &before = *library.before;
        const qint64 dune = entry(before, "Dune", "Frank Herbert");
        QVERIFY(entry(before, "Emma", "Jane Austen") > 0);
        const qint64 crows = entry(before, "The Lady of Crows", "Ann Author", "9780306406157");
        QVERIFY(before.setIdentifier(crows, "goodreads", "4671"));
        QVERIFY(library.copy());
        Library &after = *library.after;
        QVERIFY(after.remove(after.books("Emma").first().id));
        QVERIFY(entry(after, "Neuromancer", "William Gibson") > 0);
        BookMeta renamed = after.book(crows)->meta;
        renamed.title = "The Lady of Ravens";
        renamed.series = "Birds";
        QVERIFY(after.setMeta(crows, renamed));
        QVERIFY(after.setIdentifier(crows, "goodreads", "999"));
        QVERIFY(after.setIdentifier(crows, "calibre", "6f1c9b0e-0000-4000-8000-000000000001"));
        QVERIFY(after.addAnnotation(dune, highlight("The spice must flow.", 3)) > 0);
        Graph graph(after);
        const qint64 paul = graph.upsert("", "person", "Paul Atreides");
        QVERIFY(paul > 0);
        QVERIFY(graph.learn(paul, "summary", "The duke's son.") > 0);

        const LibraryDiff::Diff diff = LibraryDiff::between(before, after);
        QCOMPARE(titles(diff.added), QStringList{"Neuromancer"});
        QCOMPARE(titles(diff.removed), QStringList{"Emma"});
        QCOMPARE(titles(diff.changed), (QStringList{"Dune", "The Lady of Ravens"}));
        const auto ravens = std::ranges::find(diff.changed, QString("The Lady of Ravens"), &LibraryDiff::BookChange::title);
        QCOMPARE(ravens->id, Spaces::bookId(before, crows));
        QStringList fields;
        for (const LibraryDiff::Field &field : ravens->details)
            fields << field.field + ": " + field.before + " -> " + field.after;
        QCOMPARE(fields,
                 (QStringList{"title: The Lady of Crows -> The Lady of Ravens", "series:  -> Birds",
                              "identifiers.calibre:  -> 6f1c9b0e-0000-4000-8000-000000000001", "identifiers.goodreads: 4671 -> 999"}));
        QVERIFY(ravens->notes.isEmpty());
        const auto spice = std::ranges::find(diff.changed, QString("Dune"), &LibraryDiff::BookChange::title);
        QVERIFY(spice->details.isEmpty());
        QCOMPARE(spice->notes.value("highlight").added, 1);
        QCOMPARE(diff.notes.value("highlight").added, 1);
        QVERIFY(diff.things.added >= 1);
        QVERIFY(diff.facts.added >= 1);
        QCOMPARE(diff.things.removed, 0);

        const QJsonObject json = diff.toJson();
        QCOMPARE(json["books"]["added"].toArray().first()["title"].toString(), QString("Neuromancer"));
        QCOMPARE(json["annotations"]["highlight"]["added"].toInt(), 1);
        QVERIFY(json["graph"]["facts"]["added"].toInt() >= 1);
        // The other way round, all of it the other way round.
        const LibraryDiff::Diff back = LibraryDiff::between(after, before);
        QCOMPARE(titles(back.added), QStringList{"Emma"});
        QCOMPARE(back.notes.value("highlight").removed, 1);
        // And nothing between a library and itself.
        QVERIFY(LibraryDiff::between(after, after).empty());
        QVERIFY(!diff.empty());
    }

    // A highlight whose note changed is changed; one removed, removed.
    void annotationsAreMatchedByTheirGlobalIds() {
        Copied library;
        QVERIFY(library.before->open());
        const qint64 dune = entry(*library.before, "Dune", "Frank Herbert");
        const qint64 kept = library.before->addAnnotation(dune, highlight("Fear is the mind-killer.", 1));
        const qint64 gone = library.before->addAnnotation(dune, highlight("I must not fear.", 1));
        QVERIFY(kept > 0 && gone > 0);
        QVERIFY(library.copy());
        Library &after = *library.after;
        Annotation noted = after.annotations(dune, "highlight").first();
        QCOMPARE(noted.id, kept);
        noted.note = "The litany.";
        QVERIFY(after.storeAnnotation(dune, noted));
        after.removeAnnotation(gone);
        const auto book = LibraryDiff::book(*library.before, after, Spaces::bookId(after, dune));
        QVERIFY(book);
        QCOMPARE(book->notes.value("highlight").changed, 1);
        QCOMPARE(book->notes.value("highlight").removed, 1);
        QCOMPARE(book->notes.value("highlight").added, 0);
        QVERIFY(!LibraryDiff::book(*library.before, after, "b:nothing"));
    }

    // Put back from the earlier copy: the details alone, then the
    // annotations. A highlight removed since comes back with its own global
    // id, one made since stays; an ink page gets back the stroke erased since.
    void aBookIsPutBackAsTheEarlierCopyHadItLosingNothingMadeSince() {
        Copied library;
        QVERIFY(Fixtures::pdf(library.dir.filePath("Ferry.pdf"), "The Ferry", {"One.", "Two.", "Three."}));
        QVERIFY(library.before->open());
        Library &before = *library.before;
        const qint64 ferry = before.add(library.dir.filePath("Ferry.pdf")).id;
        QVERIFY(ferry > 0);
        QVERIFY(before.setIdentifier(ferry, "goodreads", "4671"));
        const qint64 removed = before.addAnnotation(ferry, highlight("The harbour master.", 0));
        QVERIFY(removed > 0);
        const QString hash = AnnotationAnchors::readableFile(before, ferry)->hash;
        const auto drawn = BookInk::write(before, ferry, 1, {.added = {stroke("a", 1000), stroke("b", 2000)}, .removed = {}}, hash);
        QVERIFY2(drawn, qPrintable(whyNot(drawn)));
        const QString gid = Spaces::bookId(before, ferry);
        QVERIFY(library.copy());
        Library &after = *library.after;
        const QString removedGid = Spaces::annotationId(after, removed);
        // A sync gone wrong: another book's details, and the reader's changes since.
        BookMeta wrong = after.book(ferry)->meta;
        wrong.title = "Dark Ages: Tremere";
        wrong.authors = {"Someone Else"};
        QVERIFY(after.setMeta(ferry, wrong));
        QVERIFY(after.setIdentifier(ferry, "goodreads", "999"));
        after.removeAnnotation(removed);
        QVERIFY(after.addAnnotation(ferry, highlight("Mira watched.", 1)) > 0);
        QVERIFY(BookInk::write(after, ferry, 1, {.added = {stroke("c", 3000)}, .removed = {"a"}}, hash));

        const auto details = BookRestore::restore(after, before, gid, {.details = true, .annotations = false});
        QVERIFY2(details, qPrintable(whyNot(details)));
        QVERIFY(details->details);
        QCOMPARE(details->annotations, 0);
        QCOMPARE(after.book(ferry)->meta.title, QString("The Ferry"));
        QCOMPARE(after.book(ferry)->meta.authors, before.book(ferry)->meta.authors);
        QCOMPARE(after.book(ferry)->identifiers.value("goodreads").toString(), QString("4671"));
        QCOMPARE(highlightTexts(after, ferry), QStringList{"Mira watched."});

        const auto notes = BookRestore::restore(after, before, gid, {.details = false, .annotations = true});
        QVERIFY2(notes, qPrintable(whyNot(notes)));
        QCOMPARE(notes->annotations, 1);
        QCOMPARE(notes->strokes, 1);
        QCOMPARE(highlightTexts(after, ferry), (QStringList{"Mira watched.", "The harbour master."}));
        QCOMPARE(strokeIds(after, ferry, 1), (QStringList{"a", "b", "c"}));
        const QList<Annotation> highlights = after.annotations(ferry, "highlight");
        const auto back = std::ranges::find(highlights, QString("The harbour master."), &Annotation::text);
        QCOMPARE(Spaces::annotationId(after, back->id), removedGid);
        // What is left between them is what was made since.
        const auto left = LibraryDiff::book(before, after, gid);
        QVERIFY(left);
        QVERIFY(left->details.isEmpty());
        QCOMPARE(left->notes.value("highlight").added, 1);
        QCOMPARE(left->notes.value("highlight").removed, 0);
        // Again, nothing more to put back.
        const auto again = BookRestore::restore(after, before, gid, {.details = true, .annotations = true});
        QVERIFY(again && !again->details && again->annotations == 0 && again->strokes == 0);
    }

    void aBookTheCopyLacksOrTheLibraryLostIsRefused() {
        Copied library;
        QVERIFY(library.before->open());
        const qint64 emma = entry(*library.before, "Emma", "Jane Austen");
        const QString gid = Spaces::bookId(*library.before, emma);
        QVERIFY(library.copy());
        QVERIFY(library.after->remove(emma));
        const auto lacking = BookRestore::restore(*library.after, *library.before, "b:nothing", {.details = true, .annotations = true});
        QVERIFY(!lacking);
        QCOMPARE(lacking.error().code(), Failure::Code::SnapshotBookNotFound);
        const auto lost = BookRestore::restore(*library.after, *library.before, gid, {.details = true, .annotations = true});
        QVERIFY(!lost);
        QCOMPARE(lost.error().code(), Failure::Code::RestoreBookGone);
    }
};

QTEST_MAIN(LibraryDiffTests)
#include "librarydiff_tests.moc"
