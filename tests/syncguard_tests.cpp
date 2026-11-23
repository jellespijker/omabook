// The sync guard (#187): a sync is planned on both sides and checked before
// either writes, so a sync never loses what either side had. Each of what a
// sync did wrong on the user's two real libraries in 0.2.1 and after (#158,
// #164, #166, #168: a book given another's title, authors and ISBN; ISBNs
// cleared; another book's identifier taken; a book's own identifiers removed;
// a book held twice) is a plan the guard stops; a refused sync writes nothing
// on either side; and after a sync both sides are compared with its plan.

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <algorithm>
#include <ranges>

#include "fixtures.h"
#include "hlc.h"
#include "library.h"
#include "remote.h"
#include "sync.h"
#include "syncguard.h"
#include "syncplan.h"

using SyncGuard::Rule;

namespace {

// A book as the guard sees it (SyncPlan::stateOf), without a cover or a reading.
[[nodiscard]] SyncPlan::State book(const QString &title, const QStringList &authors, const QString &isbn,
                                   const QJsonObject &identifiers = {}) {
    return {{"title", title},
            {"authors", QJsonArray::fromStringList(authors)},
            {"series", ""},
            {"series_index", 0},
            {"publisher", ""},
            {"language", ""},
            {"isbn", isbn},
            {"published", ""},
            {"description", ""},
            {"tags", QJsonArray()},
            {"identifiers", identifiers},
            {"shelf", ""},
            {"rating", 0},
            {"finished", ""},
            {"paper", false},
            {"cover", false},
            {"names", QJsonArray()}};
}

[[nodiscard]] SyncPlan::State with(SyncPlan::State state, const QString &field, const QJsonValue &value) {
    state.insert(field, value);
    return state;
}

const QString striders = QStringLiteral("Silent Striders & Black Furies");
const QString stridersIsbn = QStringLiteral("9781565048836");

[[nodiscard]] SyncPlan::State lady() { return book("The Lady of Crows", {"Anthony Ryan"}, {}, {{"goodreads", "33656533"}}); }
[[nodiscard]] SyncPlan::State silent() {
    return book(striders, {"Carl Bowen", "Gherbod Fleming"}, stridersIsbn, {{"goodreads", "1014999"}, {"isbn", stridersIsbn}});
}

// A plan of one book's change.
[[nodiscard]] SyncPlan changing(const QString &gid, const SyncPlan::State &before, const SyncPlan::State &after, bool weak) {
    SyncPlan plan;
    plan.said = true;
    plan.books.insert(gid, {.before = before, .after = after, .weak = weak});
    return plan;
}

[[nodiscard]] QList<SyncGuard::Breach> checked(const SyncPlan &plan, const SyncPlan::States &mine = {}, const SyncPlan::States &other = {},
                                               bool consent = false, const QStringList &identifiersRemoved = {}) {
    return SyncGuard::check(plan, {.mine = mine, .other = other, .consent = consent, .identifiersRemoved = identifiersRemoved});
}

[[nodiscard]] bool breaks(const QList<SyncGuard::Breach> &breaches, Rule rule, const QString &title) {
    return std::ranges::any_of(breaches, [&](const SyncGuard::Breach &breach) { return breach.rule == rule && breach.title == title; });
}

[[nodiscard]] QString said(const QList<SyncGuard::Breach> &breaches) {
    QStringList lines;
    for (const SyncGuard::Breach &breach : breaches)
        lines << SyncGuard::worded(breach, "here");
    return lines.join('\n');
}

// Every row of every table of a library's database, in a stable order.
[[nodiscard]] QMap<QString, QStringList> rowsOf(const QString &file) {
    const QString connection = "rows-" + QUuid::createUuid().toString(QUuid::Id128);
    QMap<QString, QStringList> tables;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connection);
        database.setDatabaseName(file);
        if (database.open()) {
            QSqlQuery names(database);
            names.exec("SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name");
            while (names.next())
                tables.insert(names.value(0).toString(), {});
            for (auto table = tables.begin(); table != tables.end(); ++table) {
                QSqlQuery rows(database);
                rows.exec("SELECT * FROM \"" + table.key() + "\"");
                while (rows.next()) {
                    QStringList values;
                    for (int column = 0; column < rows.record().count(); ++column)
                        values << QString::fromLatin1(rows.value(column).toByteArray().toBase64());
                    *table << values.join('|');
                }
                std::ranges::sort(*table);
            }
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return tables;
}

} // namespace

class SyncGuardTests : public QObject {
    Q_OBJECT

private slots:
    // #158: The Lady of Crows, paired with Silent Striders by identifiers it
    // had picked up and not by its id, became Silent Striders on both computers.
    void aBookIsNotRenamedAfterOneItIsPairedWithByAnIdentifier() {
        const SyncPlan plan =
            changing("e:entry:goodreads=33656533", lady(), with(silent(), "identifiers", lady().value("identifiers")), true);
        const QList<SyncGuard::Breach> breaches = checked(plan);
        QVERIFY2(breaks(breaches, Rule::BooksMerged, "The Lady of Crows"), qPrintable(said(breaches)));
        QVERIFY2(said(breaches).contains(striders), qPrintable(said(breaches)));
        // Agreed to, a rename of a book paired by an identifier is the person's to make (ADR 0009).
        QVERIFY(!breaks(checked(plan, {}, {}, true), Rule::BooksMerged, "The Lady of Crows"));
        // Of one book by its id, a newer title is its own.
        QCOMPARE(checked(changing("b:crows", lady(), with(lady(), "title", "The Lady of Crows: A Novel"), false)).size(), 0);
    }

    // #158: seven books lost their ISBN.
    void noBookLosesItsIsbn() {
        const SyncPlan plan = changing("b:silent", silent(), with(silent(), "isbn", ""), false);
        const QList<SyncGuard::Breach> breaches = checked(plan);
        QVERIFY2(breaks(breaches, Rule::FieldEmptied, striders), qPrintable(said(breaches)));
        QVERIFY2(said(breaches).contains(stridersIsbn), qPrintable(said(breaches)));
        // Even with a rename agreed to: a rename is to another name, not to none.
        QVERIFY(breaks(checked(plan, {}, {}, true), Rule::FieldEmptied, striders));
    }

    // Nor any other field it had a value in: a description, a tag, stars, a cover (#168's covers).
    void noFieldThatHadAValueIsEmptied_data() {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QJsonValue>("had");
        QTest::addColumn<QJsonValue>("emptied");
        QTest::newRow("authors") << "authors" << QJsonValue(QJsonArray{"Carl Bowen"}) << QJsonValue(QJsonArray());
        QTest::newRow("description") << "description" << QJsonValue("Garou.") << QJsonValue("");
        QTest::newRow("tags") << "tags" << QJsonValue(QJsonArray{"Fantasy"}) << QJsonValue(QJsonArray());
        QTest::newRow("shelf") << "shelf" << QJsonValue("read") << QJsonValue("");
        QTest::newRow("stars") << "rating" << QJsonValue(4) << QJsonValue(0);
        QTest::newRow("paper copy") << "paper" << QJsonValue(true) << QJsonValue(false);
        QTest::newRow("cover") << "cover" << QJsonValue(true) << QJsonValue(false);
    }

    void noFieldThatHadAValueIsEmptied() {
        QFETCH(const QString, field);
        QFETCH(const QJsonValue, had);
        QFETCH(const QJsonValue, emptied);
        const QList<SyncGuard::Breach> breaches =
            checked(changing("b:silent", with(silent(), field, had), with(silent(), field, emptied), false));
        QVERIFY2(breaks(breaches, Rule::FieldEmptied, striders), qPrintable(said(breaches)));
        QVERIFY(!checked(changing("b:silent", with(silent(), field, emptied), with(silent(), field, had), false)).size());
    }

    // #164: after an unlink, the Lady of Crows gained Silent Striders' ISBN as
    // an identifier, of a book it was paired with by less than its id.
    void aBookTakesNoIdentifierOfOneItIsPairedWithByAnIdentifier() {
        QJsonObject gained = lady().value("identifiers").toObject();
        gained.insert("isbn", stridersIsbn);
        const SyncPlan plan = changing("e:entry:goodreads=33656533", lady(), with(lady(), "identifiers", gained), true);
        QVERIFY2(breaks(checked(plan), Rule::BooksMerged, "The Lady of Crows"), qPrintable(said(checked(plan))));
        // One paired by its id takes the identifiers the other side has of it.
        QCOMPARE(checked(changing("b:crows", lady(), with(lady(), "identifiers", gained), false)).size(), 0);
    }

    // #166: a repair removed Silent Striders' own Goodreads id and ISBN, as
    // if they were of the books an older Omabook had copied them to.
    void aBooksOwnIdentifiersAreNeverRemoved() {
        const SyncPlan::States mine{{"e:entry:goodreads=1014999", silent()}};
        SyncPlan plan;
        plan.said = true;
        const QList<SyncGuard::Breach> breaches =
            checked(plan, mine, {}, false, {striders + ": isbn " + stridersIsbn, striders + ": goodreads 1014999"});
        QVERIFY2(breaks(breaches, Rule::IdentifierCleared, striders) && breaches.size() == 2, qPrintable(said(breaches)));
        QVERIFY2(said(breaches).contains("goodreads 1014999"), qPrintable(said(breaches)));
        // The ISBN the Lady of Crows has of Silent Striders is not its own, and may go, as announced.
        const SyncPlan::States crows{{"e:entry:goodreads=33656533", with(lady(), "identifiers", QJsonObject{{"isbn", stridersIsbn}})}};
        QCOMPARE(checked(plan, crows, {}, false, {"The Lady of Crows: isbn " + stridersIsbn}).size(), 0);
        // And none is cleared but as announced.
        const SyncPlan cleared = changing("e:entry:goodreads=33656533", lady(), with(lady(), "identifiers", QJsonObject()), false);
        QVERIFY(breaks(checked(cleared), Rule::IdentifierCleared, "The Lady of Crows"));
    }

    // A book held twice (#58, #154): a removal of one copy whose names the
    // other side still holds, by another id, loses a book.
    void aBookTheOtherSideStillHoldsIsNeverRemoved() {
        SyncPlan plan;
        plan.said = true;
        plan.removed.insert("e:entry:isbn=9781565041950",
                            with(book("Dark Ages: Ravnos", {}, "9781565041950"), "names", QJsonArray{"entry:isbn=9781565041950"}));
        const SyncPlan::States other{
            {"b:ravnos", with(book("Dark Ages: Ravnos", {}, "9781565041950"), "names", QJsonArray{"entry:isbn=9781565041950"})}};
        QVERIFY2(breaks(checked(plan, {}, other), Rule::BookLost, "Dark Ages: Ravnos"), qPrintable(said(checked(plan, {}, other))));
        QVERIFY(breaks(checked(plan, {}, {{"e:entry:isbn=9781565041950", book("Dark Ages: Ravnos", {}, {})}}), Rule::BookLost,
                       "Dark Ages: Ravnos"));
        // Removed there, and held by nothing else there: it goes, as the other side removed it.
        QCOMPARE(checked(plan, {}, {{"b:tremere", book("Dark Ages: Tremere", {}, {})}}).size(), 0);
    }

    // Reading places only move forward, and a bookmark never goes to another book.
    void placesAndBookmarksOnlyMoveForward() {
        SyncPlan plan;
        plan.said = true;
        plan.places << SyncPlan::Place{.title = "Kindred", .before = 2000, .after = 1000}
                    << SyncPlan::Place{.title = "Dawn", .before = 1000, .after = 2000};
        plan.annotationsMoved << "Malkavian";
        const QList<SyncGuard::Breach> breaches = checked(plan);
        QVERIFY2(breaks(breaches, Rule::PlaceBack, "Kindred") && !breaks(breaches, Rule::PlaceBack, "Dawn"), qPrintable(said(breaches)));
        QVERIFY(breaks(breaches, Rule::AnnotationMoved, "Malkavian"));
    }

    // A plan travels from the side asked as JSON, whole.
    void aPlanTravelsWhole() {
        SyncPlan plan = changing("e:entry:goodreads=33656533", lady(), silent(), true);
        plan.removed.insert("b:ravnos", book("Dark Ages: Ravnos", {}, {}));
        plan.places << SyncPlan::Place{.title = "Kindred", .before = 2000, .after = 1000};
        plan.annotationsMoved << "Malkavian";
        const SyncPlan back = SyncPlan::fromJson(plan.toJson());
        QVERIFY(back.said);
        QCOMPARE(back.toJson(), plan.toJson());
        QCOMPARE(said(checked(back)), said(checked(plan)));
        QVERIFY(!SyncPlan::fromJson({}).said);
    }

    // After the write, each side's books are what the plan announced, or the difference is said.
    void whatASyncDidIsComparedWithWhatItAnnounced() {
        const SyncPlan::States before{{"b:silent", silent()}, {"b:crows", lady()}};
        const SyncPlan plan = changing("b:crows", lady(), with(lady(), "description", "Crows."), false);
        SyncPlan::States after = before;
        after.insert("b:crows", with(lady(), "description", "Crows."));
        QCOMPARE(SyncGuard::differences(before, after, {.plan = plan, .received = 0, .identifiersRemoved = {}}), QStringList());

        after.insert("b:silent", with(silent(), "title", "Red Talons & Fianna"));
        after.remove("b:crows");
        after.insert("b:new", book("Arrived", {}, {}));
        const QString differ = SyncGuard::differences(before, after, {.plan = plan, .received = 0, .identifiersRemoved = {}}).join('\n');
        QVERIFY2(differ.contains(striders) && differ.contains("Red Talons & Fianna"), qPrintable(differ));
        QVERIFY2(differ.contains("The Lady of Crows"), qPrintable(differ));
        QVERIFY2(differ.contains("1 book more"), qPrintable(differ));

        // An identifier announced as another book's may go.
        SyncPlan::States unstrayed = before;
        unstrayed.insert("b:silent", with(silent(), "identifiers", QJsonObject{{"goodreads", "1014999"}}));
        unstrayed.insert("b:crows", with(lady(), "description", "Crows."));
        QCOMPARE(SyncGuard::differences(before, unstrayed,
                                        {.plan = plan, .received = 0, .identifiersRemoved = {striders + ": isbn " + stridersIsbn}}),
                 QStringList());
    }

    // A sync whose plan would empty a book's publisher, cleared later on the
    // other computer, is refused from either side before anything is written
    // on either; the dry run says so too; accepted for that book, it goes
    // through, and both sides are as it announced.
    void aRefusedSyncWritesNothingOnEitherSide() {
        QTemporaryDir dir;
        const QString rootA = dir.filePath("A");
        const QString rootB = dir.filePath("B");
        const QString ssh = Fixtures::standInSsh(dir.path(), OMABOOK_BINARY);
        QVERIFY(Fixtures::epub(dir.filePath("kindred.epub"), "Kindred"));
        Library a(rootA);
        Library b(rootB);
        QVERIFY(a.open() && b.open());
        const qint64 mine = a.add(dir.filePath("kindred.epub")).id;
        const qint64 theirs = b.add(dir.filePath("kindred.epub")).id;
        QVERIFY(mine > 0 && theirs > 0);
        const QString publisher = a.book(mine)->meta.publisher;
        QVERIFY(!publisher.isEmpty());
        const Remote toB("laptop:" + rootB, {ssh});
        const SyncSummary first = syncLibrary(a, toB);
        QVERIFY2(first.error.isEmpty() && first.problems.isEmpty() && first.unannounced.isEmpty(),
                 qPrintable(first.error + first.problems.join('\n') + first.unannounced.join('\n')));

        // Later, on B, the publisher is cleared: a minute on, as a sync's details are timed to the second.
        BookMeta cleared = b.book(theirs)->meta;
        cleared.publisher.clear();
        QVERIFY(b.setMeta(theirs, cleared, nullptr, Hlc::pack(QDateTime::currentSecsSinceEpoch() + 60, 0)));

        const QMap<QString, QStringList> rowsA = rowsOf(rootA + "/omabook.db");
        const QMap<QString, QStringList> rowsB = rowsOf(rootB + "/omabook.db");
        const SyncSummary dry = syncLibrary(a, toB, {}, SyncOptions{.dryRun = true});
        QVERIFY2(dry.error.isEmpty(), qPrintable(dry.error));
        QCOMPARE(dry.detailsTaken, 1);
        QVERIFY2(dry.breaches.join('\n').contains("Kindred (here)"), qPrintable(dry.breaches.join('\n')));
        const SyncSummary refused = syncLibrary(a, toB);
        QVERIFY2(refused.error.contains("Kindred") && refused.error.contains(publisher), qPrintable(refused.error));
        QCOMPARE(refused.breaches, dry.breaches);
        // And from the other computer, where the breach is of the side asked.
        const SyncSummary fromB = syncLibrary(b, Remote("laptop:" + rootA, {ssh}));
        QVERIFY2(fromB.error.contains("Kindred (on laptop)"), qPrintable(fromB.error));
        QCOMPARE(rowsOf(rootA + "/omabook.db"), rowsA);
        QCOMPARE(rowsOf(rootB + "/omabook.db"), rowsB);
        QCOMPARE(a.book(mine)->meta.publisher, publisher);

        SyncOptions accepted;
        accepted.accept = {"kindred"};
        const SyncSummary through = syncLibrary(a, toB, {}, accepted);
        QVERIFY2(through.error.isEmpty() && through.problems.isEmpty(), qPrintable(through.error + through.problems.join('\n')));
        QCOMPARE(through.breaches, dry.breaches);
        QCOMPARE(through.detailsTaken, 1);
        QVERIFY2(through.unannounced.isEmpty(), qPrintable(through.unannounced.join('\n')));
        QCOMPARE(a.book(mine)->meta.publisher, QString());
    }
};

QTEST_GUILESS_MAIN(SyncGuardTests)
#include "syncguard_tests.moc"
