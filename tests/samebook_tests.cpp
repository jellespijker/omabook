// What makes two books on two computers one book (#158, ADR 0009): its global
// id once paired, and at a first meeting only a strong identifier that names
// one book on each side, with titles that agree. Two libraries built the way
// the one that 0.2.1 got wrong is: books to read from Goodreads without an
// ISBN, Calibre's books with a uuid and its custom columns as identifiers
// ("genre", "uri", "set_id"), two different books with one ISBN, a book held
// twice, and a link an older Omabook recorded between two books that are not
// one, synced both ways over the stand-in ssh.

#include <QDateTime>
#include <QDir>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <optional>
#include <tuple>
#include <utility>

#include "bookidentity.h"
#include "booklinks.h"
#include "changelog.h"
#include "fixtures.h"
#include "hlc.h"
#include "library.h"
#include "librarymigrations.h"
#include "remote.h"
#include "replication.h"
#include "spaces.h"
#include "sync.h"

using namespace Qt::StringLiterals;

namespace {

const QString silentUuid = QStringLiteral("6f1c2a52-8d0e-4e8b-9a43-1f5b7c3d2e10");

// A book to read from Goodreads, as an import makes one: no file, "Ann
// Author", and what the last sync with Goodreads brought, the same of every one.
[[nodiscard]] qint64 toRead(Library &library, const QString &title, const QString &goodreads, const QString &isbn = {}) {
    BookMeta meta;
    meta.title = title;
    meta.authors = {"Ann Author"};
    meta.isbn = isbn;
    const qint64 id = library.addEntry(meta, false).id;
    if (id <= 0 || !library.setReading(id, "to-read", 0, {}) || !library.setIdentifier(id, "goodreads", goodreads)
        || !library.setIdentifier(id, "goodreads:synced", "to-read|0|"))
        return 0;
    return id;
}

// A book from Calibre, with its file (`file`, made once and added on each
// side that has it) and its identifiers, Calibre's custom columns among them.
[[nodiscard]] qint64 fromCalibre(Library &library, const QString &file, const QString &title, const QString &isbn,
                                 const QList<std::pair<QString, QString>> &identifiers) {
    BookMeta meta;
    meta.title = title;
    meta.authors = {"Cal Author"};
    meta.isbn = isbn;
    const qint64 id = file.isEmpty() ? library.addEntry(meta, false).id : library.add(file, meta).id;
    for (const auto &[scheme, value] : identifiers)
        if (id <= 0 || !library.setIdentifier(id, scheme, value))
            return 0;
    return id;
}

// What a book is, and which: its file's hash, else its Goodreads id.
using Identity = std::tuple<QString, QStringList, QString>;

[[nodiscard]] QString whichOf(const Library &library, const Book &book) {
    const QList<FormatFile> files = library.formatFiles(book.id);
    return files.isEmpty() ? u"goodreads:"_s + book.identifiers.value("goodreads").toString() : files.first().hash;
}

[[nodiscard]] QHash<QString, Identity> identities(const Library &library) {
    QHash<QString, Identity> books;
    for (const Book &book : library.books())
        books.insert(whichOf(library, book), Identity{book.meta.title, book.meta.authors, book.meta.isbn});
    return books;
}

// No book anywhere has a title, authors or ISBN but its own, and each side still has every book it had.
void expectOwnNames(const Library &a, const Library &b, const QHash<QString, Identity> &originalA,
                    const QHash<QString, Identity> &originalB, const QString &when) {
    QHash<QString, Identity> original = originalA;
    original.insert(originalB);
    for (const Library *library : {&a, &b}) {
        const QHash<QString, Identity> now = identities(*library);
        for (auto it = now.cbegin(); it != now.cend(); ++it)
            QVERIFY2(original.value(it.key()) == *it,
                     qPrintable(when + ": " + std::get<0>(*it) + " is not " + std::get<0>(original.value(it.key()))));
        for (const QString &which : (library == &a ? originalA : originalB).keys())
            QVERIFY2(now.contains(which), qPrintable(when + ": " + std::get<0>(original.value(which)) + " is gone"));
    }
}

[[nodiscard]] SyncSummary syncFrom(bool fromA, Library &a, Library &b, const QTemporaryDir &dir, SyncOptions options = {}) {
    const QString ssh = Fixtures::standInSsh(dir.path(), OMABOOK_BINARY);
    return fromA ? syncLibrary(a, Remote("laptop:" + dir.filePath("B"), {ssh}), {}, options)
                 : syncLibrary(b, Remote("laptop:" + dir.filePath("A"), {ssh}), {}, options);
}

// Once synced, nothing more goes either way, from either side.
void expectSettled(Library &a, Library &b, const QTemporaryDir &dir) {
    for (const bool side : {true, false}) {
        const SyncSummary settled = syncFrom(side, a, b, dir);
        QVERIFY2(settled.error.isEmpty() && settled.problems.isEmpty(), qPrintable(settled.error + settled.problems.join('\n')));
        QCOMPARE(settled.titles.detailsTaken + settled.titles.detailsSent, QStringList());
        QCOMPARE(settled.titles.renamedHere + settled.titles.renamedThere, QStringList());
        QCOMPARE(settled.received + settled.sent + settled.removedHere + settled.removedThere, 0);
        QCOMPARE(settled.readingTaken + settled.readingSent + settled.detailsUndecided, 0);
    }
}

// A dry run from one side says what one from the other does, mirrored.
void expectMirrored(const SyncSummary &fromA, const SyncSummary &fromB) {
    for (const SyncSummary *dry : {&fromA, &fromB})
        QVERIFY2(dry->error.isEmpty() && dry->problems.isEmpty(), qPrintable(dry->error + dry->problems.join('\n')));
    const auto sorted = [](QStringList titles) {
        titles.sort();
        return titles;
    };
    QCOMPARE(sorted(fromA.titles.detailsTaken), sorted(fromB.titles.detailsSent));
    QCOMPARE(sorted(fromA.titles.detailsSent), sorted(fromB.titles.detailsTaken));
    QCOMPARE(sorted(fromA.titles.renamedHere), sorted(fromB.titles.renamedThere));
    QCOMPARE(sorted(fromA.titles.renamedThere), sorted(fromB.titles.renamedHere));
    QCOMPARE(sorted(fromA.titles.received), sorted(fromB.titles.sent));
    QCOMPARE(sorted(fromA.titles.sent), sorted(fromB.titles.received));
    QCOMPARE(sorted(fromA.titles.apart), sorted(fromB.titles.apart));
}

// A same-as link, as an Omabook before 0.2.2 wrote one.
void linkAsBefore(const Library &library, const QString &one, const QString &other) {
    ChangeLog::append(library, {.space = Spaces::personalSpace(library),
                                .entity = "book",
                                .id = std::max(one, other),
                                .field = "same_as",
                                .value = std::min(one, other),
                                .hlc = std::nullopt});
}

[[nodiscard]] qsizetype countApart(const Library &library) { return BookLinks::apart(Replication::operations(library)).size(); }

} // namespace

class SameBookTests : public QObject {
    Q_OBJECT

private slots:
    // Only a valid ISBN, a Goodreads id and a Calibre uuid name a book; every
    // other identifier, a site's bookkeeping and Calibre's own number don't.
    void onlyStrongSchemesNameABook() {
        const QJsonObject columns{{"genre", "Scifan"},
                                  {"uri", "https://example.org/x"},
                                  {"set_id", "7"},
                                  {"author_origin", "nl"},
                                  {"goodreads:synced", "to-read|0|"},
                                  {"generated", "storyteller"},
                                  {"amazon", "B00LADY"},
                                  {"calibre", "42"},
                                  {"isbn", "9780140424387"}};
        QCOMPARE(BookIdentity::strongKeys(columns, "123"), QStringList());
        const QJsonObject strong{{"Goodreads", "4671.The_Lady"}, {"calibre", silentUuid.toUpper()}};
        QCOMPARE(BookIdentity::strongKeys(strong, "0-14-042438-5"),
                 (QStringList{"entry:isbn=9780140424386", "entry:goodreads=4671", "entry:calibre=" + silentUuid}));
        QCOMPARE(BookIdentity::isbn13("080442957X").value_or(QString()), QString("9780804429573"));
        QVERIFY(!BookIdentity::isbn13("0804429570"));
        QVERIFY(BookIdentity::strongId("e:entry:goodreads=1") && BookIdentity::strongId("b:abc")
                && BookIdentity::strongId("e:entry:title=a|b"));
        QVERIFY(!BookIdentity::strongId("e:entry:genre=scifan") && !BookIdentity::strongId("e:entry:calibre=42"));
    }

    void titlesAgreeButForASubtitle_data() {
        QTest::addColumn<QString>("one");
        QTest::addColumn<QString>("other");
        QTest::addColumn<bool>("agree");
        QTest::newRow("case and punctuation") << "The Lady of Crows" << "the lady of crows." << true;
        QTest::newRow("a subtitle") << "The Lady of Crows" << "The Lady of Crows: A Novel" << true;
        QTest::newRow("a series in brackets") << "Malkavian (Clanbook, #9)" << "Malkavian" << true;
        QTest::newRow("two subtitles") << "Dark Ages: Ravnos" << "Dark Ages: Tremere" << false;
        QTest::newRow("another book") << "The Lady of Crows" << "Silent Striders & Black Furies" << false;
    }

    void titlesAgreeButForASubtitle() {
        QFETCH(const QString, one);
        QFETCH(const QString, other);
        QFETCH(const bool, agree);
        QCOMPARE(BookIdentity::titlesAgree(one, other), agree);
        QCOMPARE(BookIdentity::titlesAgree(other, one), agree);
    }

    // A link between two books whose titles disagreed when it was made is
    // not followed, and once recorded as such stays dropped; one between two
    // files' ids, or two books of one title, holds.
    void aLinkBetweenTwoBooksIsJudgedByTheirTitlesThen() {
        QTemporaryDir dir;
        Library library(dir.filePath("A"));
        QVERIFY(library.open());
        const qint64 crows = toRead(library, "The Lady of Crows", "1");
        const qint64 novel = toRead(library, "The Lady of Crows: A Novel", "5");
        const qint64 striders = toRead(library, "Silent Striders & Black Furies", "2");
        QVERIFY(crows > 0 && novel > 0 && striders > 0);
        const QString crowsId = Spaces::bookId(library, crows);
        linkAsBefore(library, crowsId, Spaces::bookId(library, striders));
        linkAsBefore(library, crowsId, Spaces::bookId(library, novel));
        linkAsBefore(library, "b:aaa", "b:bbb");
        // Then the older Omabook gave the one the other's title, as it did.
        BookMeta renamed = library.book(crows)->meta;
        renamed.title = "Silent Striders & Black Furies";
        QVERIFY(library.setMeta(crows, renamed));

        const auto followed = [&library] {
            int links = 0;
            for (const SpaceOperation &op : BookLinks::trusted(Replication::operations(library)))
                links += op.field == "same_as" ? 1 : 0;
            return links;
        };
        QCOMPARE(followed(), 2);
        QCOMPARE(BookLinks::record(library, Replication::operations(library)),
                 QStringList{"Silent Striders & Black Furies / The Lady of Crows"});
        QCOMPARE(BookLinks::record(library, Replication::operations(library)), QStringList());
        QCOMPARE(countApart(library), qsizetype{1});
        QCOMPARE(followed(), 2);
    }

    // A library of 0.2.1 with such a link in its log: brought up to date, it
    // keeps the two apart, once, and says so.
    void anUpgradeDropsAWrongLinkOnce() {
        QTemporaryDir dir;
        const QString root = dir.filePath("A");
        {
            Library library(root);
            QVERIFY(library.open());
            const qint64 crows = toRead(library, "The Lady of Crows", "1");
            const qint64 tales = toRead(library, "The Canterbury Tales", "3", "9780140424386");
            linkAsBefore(library, Spaces::bookId(library, crows), Spaces::bookId(library, tales));
            QVERIFY(ChangeLog::exec(library, "PRAGMA user_version = 42"));
            QVERIFY(ChangeLog::exec(library, "DELETE FROM library_state WHERE key = 'judge_links'"));
        }
        QStringList said;
        {
            Library library(root);
            library.onBringingUpToDate([&said](const QString &line) { said << line; });
            QVERIFY(library.open());
            QCOMPARE(library.formatVersion(), LibraryMigrations::currentFormat());
            QCOMPARE(countApart(library), qsizetype{1});
        }
        QVERIFY2(said.join('\n').contains("The Canterbury Tales / The Lady of Crows"), qPrintable(said.join('\n')));
        Library again(root);
        QVERIFY(again.open());
        QCOMPARE(countApart(again), qsizetype{1});
    }

    // The two libraries, synced both ways, several times: no title, authors
    // or ISBN ever goes to another book, one sync settles them, and a dry
    // run from either side says the same.
    void librariesLikeTheUsersKeepEachBooksName_data() {
        QTest::addColumn<bool>("fromA");
        QTest::newRow("synced from A first") << true;
        QTest::newRow("synced from B first") << false;
    }

    void librariesLikeTheUsersKeepEachBooksName() {
        QFETCH(const bool, fromA);
        QTemporaryDir dir;
        const QString striders = dir.filePath("striders.epub");
        const QString ravnos = dir.filePath("ravnos.epub");
        const QString tremere = dir.filePath("tremere.epub");
        const QString twice = dir.filePath("twice.epub");
        QVERIFY(Fixtures::epub(striders, "Silent Striders & Black Furies") && Fixtures::epub(ravnos, "Dark Ages: Ravnos")
                && Fixtures::epub(tremere, "Dark Ages: Tremere") && Fixtures::epub(twice, "Twice Told"));
        Library a(dir.filePath("A"));
        Library b(dir.filePath("B"));
        QVERIFY(a.open() && b.open());
        const QString sharedIsbn = "9781565041950";
        const QList<std::pair<QString, QString>> stridersIds{{"calibre", silentUuid}, {"barnesnoble", "w/silent"},
                                                             {"google", "g0silent"},  {"genre", "Scifan"},
                                                             {"uri", "u1"},           {"set_id", "7"}};
        for (Library *library : {&a, &b}) {
            QVERIFY(fromCalibre(*library, striders, "Silent Striders & Black Furies", {}, stridersIds) > 0);
            QVERIFY(fromCalibre(*library, ravnos, "Dark Ages: Ravnos", sharedIsbn, {{"amazon", "B00DARK"}, {"genre", "Scifan"}}) > 0);
            QVERIFY(fromCalibre(*library, twice, "Twice Told", "9780000000002", {{"uri", "u1"}}) > 0);
        }
        // On A: the books to read, one of them with Silent Striders' names
        // that 0.2.0 gave it; the other book of the same ISBN; and Twice Told
        // a second time, as a Goodreads entry.
        const qint64 crows = toRead(a, "The Lady of Crows", "1");
        const QList<std::pair<QString, QString>> pickedUp{{"barnesnoble", "w/silent"}, {"google", "g0silent"}, {"calibre", silentUuid}};
        for (const auto &[scheme, value] : pickedUp)
            QVERIFY(a.setIdentifier(crows, scheme, value));
        const qint64 malkavian = toRead(a, "Malkavian", "2");
        QVERIFY(fromCalibre(a, tremere, "Dark Ages: Tremere", sharedIsbn, {{"amazon", "B00DARK"}, {"uri", "u1"}}) > 0);
        QVERIFY(toRead(a, "Twice Told", "11", "9780000000002") > 0);
        // On B: another book to read, and Tremere without its file, by the ISBN it shares with Ravnos.
        const qint64 tales = toRead(b, "The Canterbury Tales", "3", "9780140424386");
        QVERIFY(fromCalibre(b, {}, "Dark Ages: Tremere", sharedIsbn, {{"goodreads", "99"}, {"genre", "Scifan"}}) > 0);
        // And A's log has the links an older Omabook made: the Lady of Crows to Silent Striders, Malkavian to B's Tales.
        linkAsBefore(a, Spaces::bookId(a, crows), Spaces::bookId(a, a.books("Silent Striders & Black Furies").first().id));
        linkAsBefore(a, Spaces::bookId(a, malkavian), Spaces::bookId(b, tales));
        const QHash<QString, Identity> originalA = identities(a);
        const QHash<QString, Identity> originalB = identities(b);

        const SyncSummary first = syncFrom(fromA, a, b, dir);
        QVERIFY2(first.error.isEmpty() && first.problems.isEmpty(), qPrintable(first.error + first.problems.join('\n')));
        expectOwnNames(a, b, originalA, originalB, "first sync");
        expectSettled(a, b, dir);
        expectOwnNames(a, b, originalA, originalB, "settled");

        // Then each side changes a book's details.
        const qint64 later = QDateTime::currentSecsSinceEpoch() + 60;
        BookMeta described = a.book(crows)->meta;
        described.description = "Crows.";
        QVERIFY(a.setMeta(crows, described, nullptr, Hlc::pack(later, 0)));
        const qint64 theirStriders = b.books("Silent Striders & Black Furies").first().id;
        BookMeta published = b.book(theirStriders)->meta;
        published.publisher = "White Wolf";
        QVERIFY(b.setMeta(theirStriders, published, nullptr, Hlc::pack(later + 10, 0)));

        SyncOptions dry;
        dry.dryRun = true;
        const SyncSummary dryA = syncFrom(true, a, b, dir, dry);
        const SyncSummary dryB = syncFrom(false, a, b, dir, dry);
        expectMirrored(dryA, dryB);
        QCOMPARE(dryA.titles.detailsTaken, QStringList{"Silent Striders & Black Furies"});

        for (const bool side : {fromA, !fromA, fromA}) {
            const SyncSummary synced = syncFrom(side, a, b, dir);
            QVERIFY2(synced.problems.isEmpty(), qPrintable(synced.problems.join('\n')));
            expectOwnNames(a, b, originalA, originalB, "a later sync");
        }
        QCOMPARE(a.book(crows)->meta.description, QString("Crows."));
        QCOMPARE(a.books("Silent Striders & Black Furies").first().meta.publisher, QString("White Wolf"));
        for (const Book &book : b.books("The Lady of Crows"))
            QCOMPARE(book.meta.publisher, QString());
        expectSettled(a, b, dir);
    }

    // A book to read on each side, the same by its Goodreads id, one given a
    // subtitle: the first sync pairs them (their ids differ), and the newer
    // title is a rename, which both dry runs list and no sync does without
    // being asked to; nor are its other details taken until then. With
    // --accept-renames it is done, and settles.
    void aRenameOfABookPairedByAnIdentifierWaitsForConsent() {
        QTemporaryDir dir;
        Library a(dir.filePath("A"));
        Library b(dir.filePath("B"));
        QVERIFY(a.open() && b.open());
        const qint64 mine = toRead(a, "The Lady of Crows", "1");
        const qint64 theirs = toRead(b, "Lady of Crows", "1");
        QVERIFY(mine > 0 && theirs > 0);
        QVERIFY(Spaces::bookId(a, mine) != Spaces::bookId(b, theirs));
        // Then renamed there, and described, later.
        BookMeta newer = b.book(theirs)->meta;
        newer.title = "The Lady of Crows: A Novel";
        newer.description = "As Goodreads says it.";
        QVERIFY(b.setMeta(theirs, newer, nullptr, Hlc::pack(QDateTime::currentSecsSinceEpoch() + 60, 0)));

        SyncOptions dry;
        dry.dryRun = true;
        const SyncSummary dryA = syncFrom(true, a, b, dir, dry);
        const SyncSummary dryB = syncFrom(false, a, b, dir, dry);
        expectMirrored(dryA, dryB);
        const QStringList rename{"The Lady of Crows: title → The Lady of Crows: A Novel"};
        QCOMPARE(dryA.titles.renamedHere, rename);
        QCOMPARE(dryA.received + dryA.sent, 0);

        for (const bool side : {true, false}) {
            const SyncSummary synced = syncFrom(side, a, b, dir);
            QVERIFY2(synced.problems.isEmpty(), qPrintable(synced.problems.join('\n')));
            QCOMPARE(side ? synced.titles.renamedHere : synced.titles.renamedThere, rename);
            QCOMPARE(synced.renamesHeld, 1);
            QCOMPARE(a.book(mine)->meta.title, QString("The Lady of Crows"));
            // Nor its other details, until the rename is done.
            QCOMPARE(a.book(mine)->meta.description, QString());
        }
        QCOMPARE(a.books().size(), 1);
        QCOMPARE(b.books().size(), 1);

        SyncOptions accept;
        accept.acceptRenames = true;
        const SyncSummary renamed = syncFrom(true, a, b, dir, accept);
        QVERIFY2(renamed.problems.isEmpty(), qPrintable(renamed.problems.join('\n')));
        QCOMPARE(renamed.renamesHeld, 0);
        QCOMPARE(a.book(mine)->meta.title, QString("The Lady of Crows: A Novel"));
        QCOMPARE(a.book(mine)->meta.description, QString("As Goodreads says it."));
        expectSettled(a, b, dir);
    }

    // A link an older Omabook made after it had given one book the other's
    // title: the titles agreed then, so the repair keeps it, and once the
    // person put the title right a sync lists a rename it never does (#160).
    // `--unlink` with either title keeps the two apart from then on, on both
    // computers, and each keeps its own title.
    void aWrongLinkWhoseTitlesAgreedThenIsUnlinkedOnRequest_data() {
        QTest::addColumn<QString>("title");
        QTest::newRow("by the title here") << "the lady of crows";
        QTest::newRow("by the title there") << "Silent Striders & Black Furies";
    }

    void aWrongLinkWhoseTitlesAgreedThenIsUnlinkedOnRequest() {
        QFETCH(const QString, title);
        QTemporaryDir dir;
        Library a(dir.filePath("A"));
        Library b(dir.filePath("B"));
        QVERIFY(a.open() && b.open());
        const qint64 crows = toRead(a, "The Lady of Crows", "1");
        const qint64 striders = toRead(b, "Silent Striders & Black Furies", "2");
        QVERIFY(crows > 0 && striders > 0);
        // The identifier 0.2.0 gave it of the other, by which an older Omabook paired them.
        QVERIFY(a.setIdentifier(crows, "calibre", silentUuid) && b.setIdentifier(striders, "calibre", silentUuid));
        BookMeta corrupted = a.book(crows)->meta;
        corrupted.title = "Silent Striders & Black Furies";
        QVERIFY(a.setMeta(crows, corrupted));
        linkAsBefore(a, Spaces::bookId(a, crows), Spaces::bookId(b, striders));
        BookMeta putRight = a.book(crows)->meta;
        putRight.title = "The Lady of Crows";
        QVERIFY(a.setMeta(crows, putRight));
        QCOMPARE(BookLinks::record(a, Replication::operations(a)), QStringList());

        SyncOptions dry;
        dry.dryRun = true;
        const SyncSummary before = syncFrom(true, a, b, dir, dry);
        const auto renames = [](const SyncSummary &summary) { return summary.titles.renamedHere + summary.titles.renamedThere; };
        QVERIFY2(renames(before).size() == 1, qPrintable(renames(before).join('\n')));
        const SyncSummary held = syncFrom(true, a, b, dir);
        QCOMPARE(renames(held), renames(before));
        QCOMPARE(held.renamesHeld, 1);
        QCOMPARE(a.book(crows)->meta.title, QString("The Lady of Crows"));

        SyncOptions unlink;
        unlink.unlink = QStringList{title};
        const SyncSummary unlinked = syncFrom(true, a, b, dir, unlink);
        QVERIFY2(unlinked.problems.isEmpty(), qPrintable(unlinked.problems.join('\n')));
        const QStringList pair{"Silent Striders & Black Furies / The Lady of Crows"};
        QCOMPARE(unlinked.titles.unlinked, pair);
        QCOMPARE(renames(unlinked), QStringList());
        QCOMPARE(countApart(a), qsizetype{1});
        QCOMPARE(countApart(b), qsizetype{1});
        QCOMPARE(a.book(crows)->meta.title, QString("The Lady of Crows"));
        QCOMPARE(b.book(striders)->meta.title, QString("Silent Striders & Black Furies"));
        for (const bool side : {true, false}) {
            const SyncSummary later = syncFrom(side, a, b, dir, dry);
            QVERIFY2(later.problems.isEmpty(), qPrintable(later.problems.join('\n')));
            QCOMPARE(renames(later), QStringList());
        }
        QVERIFY(syncFrom(true, a, b, dir).problems.isEmpty());
        expectSettled(a, b, dir);
        QCOMPARE(a.book(crows)->meta.title, QString("The Lady of Crows"));
        QCOMPARE(b.book(striders)->meta.title, QString("Silent Striders & Black Furies"));
    }

    // A book paired by its Goodreads id, that the person renamed on purpose on
    // one computer: neither the repair nor a sync drops its link, nor does an
    // unlink of another title; the rename waits for consent, as before (#160).
    void aBookRenamedOnPurposeStaysLinked() {
        QTemporaryDir dir;
        Library a(dir.filePath("A"));
        Library b(dir.filePath("B"));
        QVERIFY(a.open() && b.open());
        const qint64 mine = toRead(a, "The Lady of Crows", "1");
        const qint64 theirs = toRead(b, "Lady of Crows", "1");
        QVERIFY(mine > 0 && theirs > 0);
        BookMeta same = b.book(theirs)->meta;
        same.title = "The Lady of Crows";
        QVERIFY(b.setMeta(theirs, same));
        const SyncSummary paired = syncFrom(true, a, b, dir);
        QVERIFY2(paired.problems.isEmpty(), qPrintable(paired.problems.join('\n')));
        QCOMPARE(paired.titles.renamedHere + paired.titles.renamedThere + paired.titles.apart, QStringList());
        BookMeta renamed = b.book(theirs)->meta;
        renamed.title = "Crows of the Lady";
        QVERIFY(b.setMeta(theirs, renamed, nullptr, Hlc::pack(QDateTime::currentSecsSinceEpoch() + 60, 0)));

        QCOMPARE(BookLinks::record(a, Replication::operations(a)), QStringList());
        QCOMPARE(BookLinks::record(b, Replication::operations(b)), QStringList());
        SyncOptions other;
        other.unlink = QStringList{"Malkavian"};
        const SyncSummary synced = syncFrom(true, a, b, dir, other);
        QCOMPARE(synced.titles.unlinked, QStringList());
        QVERIFY2(synced.problems.isEmpty(), qPrintable(synced.problems.join('\n')));
        QVERIFY2(
            synced.titles.renamedHere == QStringList{"The Lady of Crows: title → Crows of the Lady"},
            qPrintable(
                (synced.titles.renamedHere + synced.titles.renamedThere + synced.titles.apart + synced.titles.detailsTaken).join('\n')));
        QCOMPARE(synced.received + synced.sent, 0);
        QCOMPARE(countApart(a) + countApart(b), qsizetype{0});
        QCOMPARE(a.books().size(), 1);
        QCOMPARE(b.books().size(), 1);

        SyncOptions accept;
        accept.acceptRenames = true;
        const SyncSummary done = syncFrom(false, a, b, dir, accept);
        QVERIFY2(done.problems.isEmpty(), qPrintable(done.problems.join('\n')));
        QCOMPARE(a.book(mine)->meta.title, QString("Crows of the Lady"));
        expectSettled(a, b, dir);
    }

    // A book whose details are the same on both computers, while the log of
    // one says otherwise of it: a copy's older id it folds into the book (a
    // file's hash before its details were written into it, #74) has a newer
    // description, theirs newer still. A sync lists nothing of its details
    // and writes none, from either side, and the next sync neither (#160).
    void equalDetailsAreNeitherListedNorWritten() {
        QTemporaryDir dir;
        const QString file = dir.filePath("crows.epub");
        QVERIFY(Fixtures::epub(file, "The Lady of Crows"));
        Library a(dir.filePath("A"));
        Library b(dir.filePath("B"));
        QVERIFY(a.open() && b.open());
        const qint64 mine = a.add(file, {}).id;
        const qint64 theirs = b.add(file, {}).id;
        QVERIFY(mine > 0 && theirs > 0);
        const QString gid = Spaces::bookId(a, mine);
        QCOMPARE(Spaces::bookId(b, theirs), gid);
        const qint64 at = QDateTime::currentSecsSinceEpoch() + 100;
        BookMeta described = a.book(mine)->meta;
        described.description = "Crows.";
        QVERIFY(a.setMeta(mine, described, nullptr, Hlc::pack(at, 0)));
        QVERIFY(b.setMeta(theirs, described, nullptr, Hlc::pack(at + 20, 0)));
        const QString older = "b:" + QString(64, u'0');
        ChangeLog::append(a, {.space = Spaces::personalSpace(a),
                              .entity = "book",
                              .id = older,
                              .field = "description",
                              .value = "Older.",
                              .hlc = Hlc::pack(at + 10, 0)});
        linkAsBefore(a, gid, older);
        // After the first sync, which brings each side the other's operations, none is written.
        std::optional<std::pair<qsizetype, qsizetype>> logged;
        for (const bool side : {true, false, true}) {
            const SyncSummary synced = syncFrom(side, a, b, dir);
            QVERIFY2(synced.error.isEmpty() && synced.problems.isEmpty(), qPrintable(synced.error + synced.problems.join('\n')));
            QCOMPARE(synced.titles.detailsTaken + synced.titles.detailsSent, QStringList());
            QCOMPARE(synced.received + synced.sent, 0);
            QCOMPARE(a.book(mine)->meta.description, QString("Crows."));
            QCOMPARE(b.book(theirs)->meta.description, QString("Crows."));
            const std::pair now{Replication::operations(a).size(), Replication::operations(b).size()};
            if (logged)
                QCOMPARE(now, *logged);
            logged = now;
        }
    }

    // Two books to read, one on each side, with the same Goodreads id and
    // titles that disagree: never paired, and said to be kept apart. Each
    // side keeps its own: that identifier names a book there, so neither is
    // fetched as one the other lacks.
    void anIdentifierWithTitlesThatDisagreePairsNothing() {
        QTemporaryDir dir;
        Library a(dir.filePath("A"));
        Library b(dir.filePath("B"));
        QVERIFY(a.open() && b.open());
        QVERIFY(toRead(a, "The Lady of Crows", "1") > 0);
        QVERIFY(toRead(b, "Silent Striders & Black Furies", "1") > 0);
        const SyncSummary synced = syncFrom(true, a, b, dir);
        QVERIFY2(synced.problems.isEmpty(), qPrintable(synced.problems.join('\n')));
        QCOMPARE(synced.titles.apart, QStringList{"Silent Striders & Black Furies / The Lady of Crows (goodreads=1)"});
        QCOMPARE(a.books("The Lady of Crows").size(), 1);
        QCOMPARE(b.books("Silent Striders & Black Furies").size(), 1);
        QCOMPARE(b.books("The Lady of Crows").size(), 0);
    }
};

QTEST_MAIN(SameBookTests)
#include "samebook_tests.moc"
