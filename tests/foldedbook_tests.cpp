// Folding a duplicate into the other copy keeps what only the folded one had,
// names what it would not keep and leaves that to the person, and the window's
// Tidy takes a moment snapshot first, changing nothing when it can't (#282).

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QtTest>

#include "curation.h"
#include "desk.h"
#include "library.h"
#include "mcpharness.h"
#include "snapshots.h"

using namespace Qt::StringLiterals;

namespace {

// A book in a file with no details, and one on paper with the same title and details typed by hand.
[[nodiscard]] std::pair<qint64, qint64> twoCopies(Library &library, const QString &folder) {
    const QString file = folder + "/Gull Manual.md";
    if (!Fixtures::write(file, "# Gulls\n"))
        return {};
    const qint64 kept = library.add(file, BookMeta{.title = u"Gull Manual"_s, .authors = {u"Pia Pdf"_s}}).id;
    const qint64 folded = library
                              .addEntry(BookMeta{.title = u"The Gull Manual"_s,
                                                 .authors = {u"Pia Pdf"_s},
                                                 .series = u"Gulls"_s,
                                                 .seriesIndex = 2,
                                                 .publisher = u"Shore Press"_s,
                                                 .isbn = u"9780306406157"_s,
                                                 .description = u"My own description, typed by hand"_s,
                                                 .tags = {u"birds"_s, u"favourite"_s}},
                                        true)
                              .id;
    return {kept, folded};
}

void hasTheDetails(const Book &book) {
    QCOMPARE(book.meta.description, u"My own description, typed by hand"_s);
    QCOMPARE(book.meta.tags, (QStringList{u"birds"_s, u"favourite"_s}));
    QCOMPARE(book.meta.publisher, u"Shore Press"_s);
    QCOMPARE(book.meta.isbn, u"9780306406157"_s);
    QCOMPARE(book.meta.series, u"Gulls"_s);
    QCOMPARE(book.meta.seriesIndex, 2.0);
}

} // namespace

class FoldedBookTests : public QObject {
    Q_OBJECT

private slots:
    void whatOnlyTheFoldedBookHadIsKept() {
        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const auto [kept, folded] = twoCopies(library, dir.path());
        const QList<Proposal> proposals = Curation::duplicates(library.books());
        QCOMPARE(proposals.size(), 1);
        QCOMPARE(proposals.first().bookId, kept);
        QVERIFY(!proposals.first().waits());
        QString why;
        QVERIFY2(Curation::apply(library, proposals.first(), &why), qPrintable(why));
        QVERIFY(!library.book(folded));
        const Book book = *library.book(kept);
        hasTheDetails(book);
        QCOMPARE(book.meta.title, u"Gull Manual"_s);
        QVERIFY(book.paper);
        QVERIFY(QFileInfo::exists(library.formatPath(kept)));
    }

    void tagsOfBothAreKept() {
        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const qint64 kept = library.addEntry({.title = u"Moss"_s, .description = u"Green."_s, .tags = {u"one"_s, u"Both"_s}}, true).id;
        const qint64 folded = library.addEntry({.title = u"Moss"_s, .language = u"en"_s, .tags = {u"both"_s, u"two"_s}}, true).id;
        QVERIFY(library.merge(kept, folded));
        QCOMPARE(library.book(kept)->meta.tags, (QStringList{u"one"_s, u"Both"_s, u"two"_s}));
        QCOMPARE(library.book(kept)->meta.language, u"en"_s);
        QCOMPARE(library.book(kept)->meta.description, u"Green."_s);
    }

    void whatWouldNotBeKeptIsNamedAndLeftToThePerson() {
        const McpHarness::Machine machine;
        {
            Library library(machine.library);
            QVERIFY(library.open());
            QCOMPARE(library.addEntry({.title = u"Moss"_s, .publisher = u"Shore"_s, .description = u"Green."_s}, true).id, 1);
            QCOMPARE(library.addEntry({.title = u"Moss"_s, .publisher = u"Harbour"_s, .description = u"Grey."_s}, true).id, 2);
        }
        const auto looked = machine.omabook({"tidy", "--library", machine.library});
        QVERIFY2(looked.output.contains("would not keep book 2's publisher, description"), qPrintable(looked.output));
        const QJsonObject json =
            QJsonDocument::fromJson(machine.omabook({"tidy", "--json", "--library", machine.library}).output.toUtf8()).object();
        const QJsonObject proposal = json.value("proposals").toArray().first().toObject();
        QCOMPARE(proposal.value("confidence").toString(), u"low"_s);
        QCOMPARE(proposal.value("drops").toArray(), (QJsonArray{u"publisher"_s, u"description"_s}));
        // Not by a blind apply; by its number, as the person's own choice.
        QCOMPARE(machine.omabook({"tidy", "--apply", "--library", machine.library}).code, 0);
        QVERIFY(machine.omabook({"show", "2", "--library", machine.library}).output.contains("Grey."));
        QCOMPARE(machine.omabook({"tidy", "--pick", "1", "--library", machine.library}).code, 0);
        Library library(machine.library);
        QVERIFY(library.open());
        QCOMPARE(library.books().size(), 1);
        QCOMPARE(library.book(1)->meta.description, u"Green."_s);
    }

    void aSecondFoldIsJudgedAgainstWhatTheFirstBrought() {
        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        QVERIFY(library.addEntry({.title = u"Moss"_s, .tags = {u"a"_s}}, true).id > 0);
        QVERIFY(library.addEntry({.title = u"Moss"_s, .publisher = u"Shore"_s}, true).id > 0);
        QVERIFY(library.addEntry({.title = u"Moss"_s, .publisher = u"Harbour"_s}, true).id > 0);
        const QList<Proposal> proposals = Curation::duplicates(library.books());
        QCOMPARE(proposals.size(), 2);
        QVERIFY(proposals.at(0).dropped.isEmpty());
        QCOMPARE(proposals.at(1).dropped, QStringList{u"publisher"_s});
    }

    void theWindowTakesAMomentSnapshotBeforeItFolds() {
        QVERIFY(!QStandardPaths::findExecutable(u"restic"_s).isEmpty());
        QTemporaryDir state, repository;
        qputenv("XDG_STATE_HOME", state.path().toUtf8());
        Desk desk("en", {}, false);
        Library *library = desk.backend->library();
        const auto [kept, folded] = twoCopies(*library, desk.dir.path());
        Snapshots snapshots(SnapshotPlace::here());
        QVERIFY(snapshots.setUp(repository.filePath("repository")).has_value());
        QSignalSpy applied(&*desk.librarian, &Librarian::applied);

        // Closed while the snapshot is taken: nothing is changed.
        desk.librarian->tidy();
        QTRY_VERIFY(!desk.librarian->busy());
        desk.librarian->applyChosen();
        desk.librarian->stop();
        QTest::qWait(3000);
        QCOMPARE(applied.size(), 0);
        QVERIFY(library->book(folded));

        desk.librarian->tidy();
        QTRY_VERIFY(!desk.librarian->busy());
        desk.librarian->applyChosen();
        QTRY_COMPARE_WITH_TIMEOUT(applied.size(), 1, 60000);
        QVERIFY(!library->book(folded));
        hasTheDetails(*library->book(kept));
        const auto listed = snapshots.list(*library);
        QVERIFY(listed.has_value());
        QVERIFY(!listed->isEmpty());
        QCOMPARE(listed->last().tagged(u"reason:"_s), Snapshots::reasonName(SnapshotReason::Tidy));
    }

    void withNoSnapshotToBeHadTheWindowChangesNothing() {
        QTemporaryDir state, repository;
        qputenv("XDG_STATE_HOME", state.path().toUtf8());
        Desk desk("en", {}, false);
        Library *library = desk.backend->library();
        const auto copies = twoCopies(*library, desk.dir.path());
        Snapshots snapshots(SnapshotPlace::here());
        QVERIFY(snapshots.setUp(repository.filePath("repository")).has_value());
        QVERIFY(QDir(repository.filePath("repository")).removeRecursively());
        desk.librarian->tidy();
        QTRY_VERIFY(!desk.librarian->busy());
        QSignalSpy applied(&*desk.librarian, &Librarian::applied);
        QSignalSpy busy(&*desk.librarian, &Librarian::busyChanged);
        desk.librarian->applyChosen();
        QTRY_VERIFY_WITH_TIMEOUT(!desk.librarian->busy(), 60000);
        QCOMPARE(applied.size(), 0);
        QVERIFY(library->book(copies.second));
        QCOMPARE(desk.librarian->chosen(), 1);
    }

    // Set up once, and the password gone since: not the same as never set up (#296).
    void withTheSnapshotPasswordGoneTheWindowChangesNothing() {
        QVERIFY(!QStandardPaths::findExecutable(u"restic"_s).isEmpty());
        QTemporaryDir state, repository;
        qputenv("XDG_STATE_HOME", state.path().toUtf8());
        Desk desk("en", {}, false);
        Library *library = desk.backend->library();
        const auto copies = twoCopies(*library, desk.dir.path());
        Snapshots snapshots(SnapshotPlace::here());
        QVERIFY(snapshots.setUp(repository.filePath("repository")).has_value());
        QVERIFY(QFile::remove(SnapshotPlace::here().repository.passwordFile));
        desk.librarian->tidy();
        QTRY_VERIFY(!desk.librarian->busy());
        QSignalSpy applied(&*desk.librarian, &Librarian::applied);
        desk.librarian->applyChosen();
        QVERIFY(!desk.librarian->busy());
        QCOMPARE(applied.size(), 0);
        QVERIFY(library->book(copies.second));
        QCOMPARE(desk.librarian->chosen(), 1);
        const QString said = desk.backend->message();
        QVERIFY2(said.contains(u"Nothing was changed"_s) && said.contains(u"password isn't in"_s), qPrintable(said));
    }

    // A copy folded away is gone from the library: a restore of it says so, by
    // its title too, rather than that the title is ambiguous and no more, or
    // that there is nothing to put back (#282).
    void aFoldedBookIsSaidToHaveLeftTheLibrary() {
        QVERIFY(!QStandardPaths::findExecutable(u"restic"_s).isEmpty());
        const McpHarness::Machine machine;
        {
            Library library(machine.library);
            QVERIFY(library.open());
            const QString file = machine.dir.filePath("Gull Manual.md");
            QVERIFY(Fixtures::write(file, "# Gulls\n"));
            QCOMPARE(library.add(file, BookMeta{.title = u"Gull Manual"_s, .authors = {u"Pia Pdf"_s}}).id, 1);
            QCOMPARE(library.addEntry({.title = u"Gull Manual"_s, .authors = {u"Pia Pdf"_s}, .description = u"Typed."_s}, true).id, 2);
        }
        const auto run = [&machine](const QStringList &arguments) {
            return machine.omabook(arguments + QStringList{u"--library"_s, machine.library});
        };
        QCOMPARE(run({u"snapshot"_s, u"setup"_s, u"--repository"_s, machine.dir.filePath("repository")}).code, 0);
        QCOMPARE(run({u"tidy"_s, u"--apply"_s}).code, 0);
        const auto listed = run({u"snapshot"_s, u"list"_s, u"--json"_s});
        const QString id =
            QJsonDocument::fromJson(listed.output.toUtf8()).object().value("snapshots").toArray().last().toObject().value("id").toString();
        QVERIFY2(!id.isEmpty(), qPrintable(listed.output + listed.errors));

        const auto called = run({u"snapshot"_s, u"restore"_s, id, u"--book"_s, u"Gull Manual"_s, u"--yes"_s});
        QCOMPARE(called.code, 1);
        QString folded;
        for (const QString &line : called.errors.split(u'\n'))
            if (line.contains(u"no longer in the library"_s))
                folded = line.trimmed().section(u'\t', 0, 0);
        QVERIFY2(!folded.isEmpty(), qPrintable(called.errors));

        const auto restored = run({u"snapshot"_s, u"restore"_s, id, u"--book"_s, folded, u"--yes"_s});
        QCOMPARE(restored.code, 1);
        QVERIFY2(restored.errors.contains(u"no longer in this library"_s) && restored.errors.contains(u"new folder"_s),
                 qPrintable(restored.output + restored.errors));
        QVERIFY2(!restored.output.contains(u"Nothing to put back"_s), qPrintable(restored.output));
    }
};

QTEST_MAIN(FoldedBookTests)
#include "foldedbook_tests.moc"
