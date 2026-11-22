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
#include <QJsonDocument>
#include <QJsonObject>
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

    // A library of 0.3.0, where a book kept apart from another has its ISBN,
    // which the Lady of Crows gained of Silent Striders after #160's unlink
    // (#164): brought up to date, it loses that identifier, once, and says
    // so; the other book keeps its own.
    void anUpgradeRemovesAnIdentifierOfABookKeptApart() {
        QTemporaryDir dir;
        const QString root = dir.filePath("A");
        const QString isbn = "9781565048836";
        {
            Library library(root);
            QVERIFY(library.open());
            const qint64 striders = toRead(library, "Silent Striders & Black Furies", "2", isbn);
            QVERIFY(striders > 0 && library.setIdentifier(striders, "isbn", isbn));
            const qint64 crows = toRead(library, "The Lady of Crows", "1");
            QVERIFY(crows > 0 && library.setIdentifier(crows, "isbn", isbn));
            ChangeLog::append(library, {.space = Spaces::personalSpace(library),
                                        .entity = "book",
                                        .id = std::max(Spaces::bookId(library, crows), Spaces::bookId(library, striders)),
                                        .field = "not_same_as",
                                        .value = std::min(Spaces::bookId(library, crows), Spaces::bookId(library, striders)),
                                        .hlc = std::nullopt});
            QVERIFY(ChangeLog::exec(library, "PRAGMA user_version = 43"));
            QVERIFY(ChangeLog::exec(library, "DELETE FROM library_state WHERE key = 'unstray_identifiers'"));
        }
        QStringList said;
        {
            Library library(root);
            library.onBringingUpToDate([&said](const QString &line) { said << line; });
            QVERIFY(library.open());
            QCOMPARE(library.formatVersion(), LibraryMigrations::currentFormat());
            QVERIFY(!library.books("The Lady of Crows").first().identifiers.contains("isbn"));
            QCOMPARE(library.books("Silent Striders & Black Furies").first().identifiers.value("isbn").toString(), isbn);
        }
        QVERIFY2(said.join('\n').contains("The Lady of Crows: isbn " + isbn), qPrintable(said.join('\n')));
        said.clear();
        Library again(root);
        again.onBringingUpToDate([&said](const QString &line) { said << line; });
        QVERIFY(again.open());
        QCOMPARE(said, QStringList());
        QCOMPARE(again.books("Silent Striders & Black Furies").first().identifiers.value("isbn").toString(), isbn);
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

    // The case of #164: a Goodreads entry without an ISBN on A, linked by an
    // older Omabook (as #160's test builds it) to a book with one on B. Both
    // hold both books, as syncs with that Omabook left them, and B's copy of
    // the entry was given the other's ISBN as an identifier then. In the run
    // that unlinks them, from either side, and after, no book gains anything
    // of the other's, identifiers included; and the ISBN B's copy got from the
    // book now kept apart is removed, and said to be.
    void anUnlinkedPairGainsNothingOfTheOther_data() {
        QTest::addColumn<bool>("fromA");
        QTest::newRow("unlinked from A") << true;
        QTest::newRow("unlinked from B") << false;
    }

    void anUnlinkedPairGainsNothingOfTheOther() {
        QFETCH(const bool, fromA);
        QTemporaryDir dir;
        Library a(dir.filePath("A"));
        Library b(dir.filePath("B"));
        QVERIFY(a.open() && b.open());
        const QString isbn = "9781565048836";
        const QString title = "Silent Striders & Black Furies";
        const qint64 striders = toRead(b, title, "2", isbn);
        const qint64 theirStriders = toRead(a, title, "2", isbn);
        const qint64 crows = toRead(a, "The Lady of Crows", "1");
        const qint64 copy = toRead(b, "The Lady of Crows", "1");
        QVERIFY(striders > 0 && theirStriders > 0 && crows > 0 && copy > 0);
        QVERIFY(b.setIdentifier(striders, "isbn", isbn) && a.setIdentifier(theirStriders, "isbn", isbn));
        QCOMPARE(Spaces::bookId(a, crows), Spaces::bookId(b, copy));
        QCOMPARE(Spaces::bookId(a, theirStriders), Spaces::bookId(b, striders));
        // What the older Omabook did: B's copy of the entry took the other's ISBN, and A's its title, when it linked them.
        // Each a while after what came before, on either computer's clock.
        QVERIFY(b.setIdentifier(copy, "isbn", isbn));
        const qint64 later = QDateTime::currentSecsSinceEpoch() + 20;
        BookMeta corrupted = a.book(crows)->meta;
        corrupted.title = title;
        QVERIFY(a.setMeta(crows, corrupted, nullptr, Hlc::pack(later, 0)));
        ChangeLog::append(a, {.space = Spaces::personalSpace(a),
                              .entity = "book",
                              .id = std::max(Spaces::bookId(a, crows), Spaces::bookId(b, striders)),
                              .field = "same_as",
                              .value = std::min(Spaces::bookId(a, crows), Spaces::bookId(b, striders)),
                              .hlc = Hlc::pack(later + 10, 0)});
        BookMeta putRight = a.book(crows)->meta;
        putRight.title = "The Lady of Crows";
        QVERIFY(a.setMeta(crows, putRight, nullptr, Hlc::pack(later + 20, 0)));
        QCOMPARE(BookLinks::record(a, Replication::operations(a)), QStringList());
        QJsonObject copyOwn = b.book(copy)->identifiers;
        const QList<std::tuple<Library *, qint64, Book>> books{
            {&a, crows, *a.book(crows)}, {&a, theirStriders, *a.book(theirStriders)}, {&b, striders, *b.book(striders)}};
        const auto expectOwn = [&](const QString &when, bool repaired) {
            for (const auto &[library, id, before] : books) {
                const std::optional<Book> now = library->book(id);
                QVERIFY2(now, qPrintable(when + ": " + before.meta.title + " is gone"));
                QVERIFY2(now->identifiers == before.identifiers,
                         qPrintable(when + ": " + before.meta.title + " has "
                                    + QString::fromUtf8(QJsonDocument(now->identifiers).toJson(QJsonDocument::Compact))));
                QCOMPARE((Identity{now->meta.title, now->meta.authors, now->meta.isbn}),
                         (Identity{before.meta.title, before.meta.authors, before.meta.isbn}));
            }
            if (repaired)
                copyOwn.remove("isbn");
            QCOMPARE(b.book(copy)->identifiers, copyOwn);
        };
        QVERIFY(syncFrom(true, a, b, dir).problems.isEmpty());
        expectOwn("a sync before the unlink", false);

        SyncOptions unlink;
        unlink.unlink = QStringList{title};
        const SyncSummary unlinked = syncFrom(fromA, a, b, dir, unlink);
        QVERIFY2(unlinked.problems.isEmpty(), qPrintable(unlinked.problems.join('\n')));
        QCOMPARE(unlinked.titles.unlinked, QStringList{title + " / The Lady of Crows"});
        expectOwn("the sync that unlinks", true);
        QCOMPARE(unlinked.titles.identifiersRemoved, QStringList{"The Lady of Crows: isbn " + isbn});
        for (const bool side : {!fromA, fromA}) {
            const SyncSummary later = syncFrom(side, a, b, dir);
            QVERIFY2(later.problems.isEmpty(), qPrintable(later.problems.join('\n')));
            QCOMPARE(later.titles.identifiersRemoved, QStringList());
            expectOwn("a later sync", false);
        }
    }

    // Two books paired at their first meeting by their Goodreads id, each
    // with its own other names: their ids differ, so no identifier goes from
    // one to the other, from either side, at that sync or any later one (#164).
    void noIdentifierMovesBetweenBooksPairedByOne() {
        QTemporaryDir dir;
        Library a(dir.filePath("A"));
        Library b(dir.filePath("B"));
        QVERIFY(a.open() && b.open());
        const qint64 mine = toRead(a, "The Lady of Crows", "1");
        const qint64 theirs = toRead(b, "The Lady of Crows", "1", "9781565048836");
        QVERIFY(mine > 0 && theirs > 0);
        QVERIFY(a.setIdentifier(mine, "amazon", "B00CROWS") && b.setIdentifier(theirs, "calibre", silentUuid));
        QVERIFY(Spaces::bookId(a, mine) != Spaces::bookId(b, theirs));
        const QJsonObject ours = a.book(mine)->identifiers;
        const QJsonObject others = b.book(theirs)->identifiers;
        for (const bool side : {true, false, true}) {
            const SyncSummary synced = syncFrom(side, a, b, dir);
            QVERIFY2(synced.problems.isEmpty(), qPrintable(synced.problems.join('\n')));
            QCOMPARE(a.books().size(), 1);
            QCOMPARE(b.books().size(), 1);
            QCOMPARE(a.book(mine)->identifiers, ours);
            QCOMPARE(b.book(theirs)->identifiers, others);
        }
    }

    // The shape of #166, with titles of its own: a book with a file (an
    // orchard), its Goodreads entry, a second book with its file and entry (a
    // hollow), and a third book's entry (a lantern). An older Omabook linked
    // the hollow's entry and the orchard's to the lantern's (by bookkeeping,
    // their titles disagreeing), gave the lantern the orchard's title and
    // names elsewhere and the hollow's authors, linked it so to the orchard's
    // file (the titles agreeing then), and left the orchard's file with no
    // ISBN in the log. The person put the lantern's title right. Brought up to
    // date, the library keeps each book's own ISBN and Goodreads id, and loses
    // only the ISBN the lantern had of the orchard.
    void anUpgradeKeepsEachBooksOwnIdentifiers() {
        QTemporaryDir dir;
        const QString root = dir.filePath("A");
        const QString orchardIsbn = "9780000001115";
        const QString hollowIsbn = "9780000002228";
        const QString orchardUuid = "2b7d4e1a-5c3f-4a9b-8e60-7d1c2f3a4b5c";
        const QString orchardFile = dir.filePath("orchard.epub");
        const QString hollowFile = dir.filePath("hollow.epub");
        QVERIFY(Fixtures::epub(orchardFile, "The Glass Orchard") && Fixtures::epub(hollowFile, "Copper Hollow"));
        QHash<QString, Book> before;
        {
            Library library(root);
            QVERIFY(library.open());
            const qint64 orchardEntry = toRead(library, "The Glass Orchard", "5001", orchardIsbn);
            const qint64 hollowEntry = toRead(library, "Copper Hollow", "5002", hollowIsbn);
            const qint64 lantern = toRead(library, "The Salt Lantern", "5003");
            const qint64 orchard = fromCalibre(library, orchardFile, "The Glass Orchard", orchardIsbn,
                                               {{"isbn", orchardIsbn}, {"goodreads", "5001"}, {"calibre", orchardUuid}});
            const qint64 hollow =
                fromCalibre(library, hollowFile, "Copper Hollow", hollowIsbn, {{"isbn", hollowIsbn}, {"goodreads", "5002"}});
            QVERIFY(orchardEntry > 0 && hollowEntry > 0 && lantern > 0 && orchard > 0 && hollow > 0);
            const QString lanternId = Spaces::bookId(library, lantern);
            const QString orchardId = Spaces::bookId(library, orchard);
            linkAsBefore(library, Spaces::bookId(library, hollowEntry), lanternId);
            linkAsBefore(library, lanternId, Spaces::bookId(library, orchardEntry));
            BookMeta corrupted = library.book(lantern)->meta;
            corrupted.title = "The Glass Orchard";
            corrupted.authors = library.book(hollow)->meta.authors;
            QVERIFY(library.setMeta(lantern, corrupted));
            QVERIFY(library.setIdentifier(lantern, "isbn", orchardIsbn) && library.setIdentifier(lantern, "calibre", orchardUuid));
            ChangeLog::append(library, {.space = Spaces::personalSpace(library),
                                        .entity = "book",
                                        .id = orchardId,
                                        .field = "isbn",
                                        .value = {},
                                        .hlc = std::nullopt});
            linkAsBefore(library, lanternId, orchardId);
            BookMeta putRight = library.book(lantern)->meta;
            putRight.title = "The Salt Lantern";
            putRight.authors = QStringList{"Ann Author"};
            QVERIFY(library.setMeta(lantern, putRight));
            for (const Book &book : library.books())
                before.insert(Spaces::bookId(library, book.id), book);
            QVERIFY(ChangeLog::exec(library, "PRAGMA user_version = 42"));
            QVERIFY(ChangeLog::exec(library, "DELETE FROM library_state WHERE key IN ('judge_links', 'unstray_identifiers')"));
        }
        QStringList said;
        Library library(root);
        library.onBringingUpToDate([&said](const QString &line) { said << line; });
        QVERIFY(library.open());
        QCOMPARE(library.formatVersion(), LibraryMigrations::currentFormat());
        QVERIFY2(!said.join('\n').contains("The Glass Orchard:"), qPrintable(said.join('\n')));
        QVERIFY2(
            said.join('\n').contains("Removed 1 identifier that a book had of another an older Omabook took it for: The Salt Lantern: isbn "
                                     + orchardIsbn + "."),
            qPrintable(said.join('\n')));
        QCOMPARE(library.books().size(), before.size());
        for (const Book &book : library.books()) {
            const Book was = before.value(Spaces::bookId(library, book.id));
            QCOMPARE((Identity{book.meta.title, book.meta.authors, book.meta.isbn}),
                     (Identity{was.meta.title, was.meta.authors, was.meta.isbn}));
            QJsonObject own = was.identifiers;
            if (was.meta.title == "The Salt Lantern")
                own.remove("isbn");
            QVERIFY2(
                book.identifiers == own,
                qPrintable(book.meta.title + ": " + QString::fromUtf8(QJsonDocument(book.identifiers).toJson(QJsonDocument::Compact))));
        }
    }

    // The link behind the rename #166 found: an older Omabook linked the
    // lantern's entry to the orchard's when it had given it the orchard's
    // title, on both computers. The person put the lantern right on A; B has
    // it right too, but its log still says the orchard's title. From B, `--unlink` finds the link by the title the
    // book has there, and the sync keeps both books' names.
    void anUnlinkFindsTheLinkByTheTitleABookHasHere() {
        QTemporaryDir dir;
        Library a(dir.filePath("A"));
        Library b(dir.filePath("B"));
        QVERIFY(a.open() && b.open());
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        const qint64 lantern = toRead(a, "The Salt Lantern", "5003");
        const qint64 copy = toRead(b, "The Salt Lantern", "5003");
        const qint64 orchard = toRead(b, "The Glass Orchard", "5001", "9780000001115");
        QVERIFY(lantern > 0 && copy > 0 && orchard > 0 && toRead(a, "The Glass Orchard", "5001", "9780000001115") > 0);
        const QString lanternId = Spaces::bookId(b, copy);
        QCOMPARE(Spaces::bookId(a, lantern), lanternId);
        BookMeta corrupted = a.book(lantern)->meta;
        corrupted.title = "The Glass Orchard";
        QVERIFY(a.setMeta(lantern, corrupted, nullptr, Hlc::pack(now + 10, 0)));
        // What an older Omabook left in B's log, and never in the book there.
        ChangeLog::append(b, {.space = Spaces::personalSpace(b),
                              .entity = "book",
                              .id = lanternId,
                              .field = "title",
                              .value = "The Glass Orchard",
                              .hlc = Hlc::pack(now + 10, 0)});
        for (const Library *library : {&a, &b})
            ChangeLog::append(*library, {.space = Spaces::personalSpace(*library),
                                         .entity = "book",
                                         .id = std::max(lanternId, Spaces::bookId(b, orchard)),
                                         .field = "same_as",
                                         .value = std::min(lanternId, Spaces::bookId(b, orchard)),
                                         .hlc = Hlc::pack(now + 20, 0)});
        BookMeta putRight = a.book(lantern)->meta;
        putRight.title = "The Salt Lantern";
        QVERIFY(a.setMeta(lantern, putRight, nullptr, Hlc::pack(now + 60, 0)));
        QCOMPARE(BookLinks::record(b, Replication::operations(b)), QStringList());
        QCOMPARE(b.book(copy)->meta.title, QString("The Salt Lantern"));
        const QStringList pair{"The Glass Orchard / The Salt Lantern"};
        QCOMPARE(BookLinks::unlink(b, Replication::operations(b), "The Salt Lantern", true), pair);
        SyncOptions unlink;
        unlink.unlink = QStringList{"The Salt Lantern"};
        const SyncSummary unlinked = syncFrom(false, a, b, dir, unlink);
        QVERIFY2(unlinked.problems.isEmpty(), qPrintable(unlinked.problems.join('\n')));
        QCOMPARE(unlinked.titles.unlinked, pair);
        QCOMPARE(countApart(a), qsizetype{1});
        QCOMPARE(countApart(b), qsizetype{1});
        for (const Library *library : {&a, &b}) {
            QCOMPARE(library->books("The Salt Lantern").size(), 1);
            QCOMPARE(library->books("The Glass Orchard").size(), 1);
            QCOMPARE(library->books("The Glass Orchard").first().identifiers.value("goodreads").toString(), QString("5001"));
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
