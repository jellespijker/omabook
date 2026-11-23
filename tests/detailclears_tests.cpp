// A change or a clear of a book's details on one computer reaches the other
// (#210), for a book on paper (no file) as for a book with a file, whichever
// side it was made on: two temporary libraries, one book made on both or that
// went from one to the other by a sync, a field changed or cleared on one
// later, and synced again over the stand-in ssh. A clear empties a field that
// had a value, so the sync guard (#187) refuses it until the person accepts it
// for that book; then it arrives as the plan announced, and nothing
// unannounced happens.
//
// A computer that took a book's details by a sync wrote none of them into the
// change log as its own, as they were there already: to the other computer its
// book had none of them, and a change made there of one went nowhere.

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include <tuple>

#include "fixtures.h"
#include "hlc.h"
#include "library.h"
#include "remote.h"
#include "sync.h"
#include "syncplan.h"

namespace {

const QString title = QStringLiteral("Clanbook: Ventrue");

// Every detail a person can give a book.
[[nodiscard]] BookMeta details() {
    BookMeta meta;
    meta.title = title;
    meta.authors = {"Ann Author"};
    meta.isbn = "9781565048836";
    meta.series = "Clanbooks";
    meta.seriesIndex = 3;
    meta.description = "The clan of kings.";
    meta.publisher = "White Wolf";
    meta.tags = {"Vampire"};
    return meta;
}

// The book, on paper or of a file in `folder`, with a Goodreads id and read.
[[nodiscard]] qint64 made(Library &library, bool paper, const QString &folder) {
    qint64 id = 0;
    if (paper)
        id = library.addEntry(details(), true).id;
    else if (Fixtures::epub(folder + "/ventrue.epub", title))
        id = library.add(folder + "/ventrue.epub", details()).id;
    return id > 0 && library.setIdentifier(id, "goodreads", "1014999") && library.setReading(id, "read", 4, {}) ? id : 0;
}

// The field as the guard sees a book (SyncPlan::stateOf).
[[nodiscard]] QJsonValue fieldOf(const Library &library, qint64 id, const QString &field) {
    return SyncPlan::stateOf(library, *library.book(id)).value(field);
}

// Changes `field` of a book to `value` (empty: clears it), a minute on, as a sync's details are timed to the second.
[[nodiscard]] bool change(Library &library, qint64 id, const QString &field, const QString &value) {
    const qint64 later = Hlc::pack(QDateTime::currentSecsSinceEpoch() + 60, 0);
    BookMeta meta = library.book(id)->meta;
    QJsonObject json = meta.toJson();
    json.insert(field, field == "authors" || field == "tags" ? QJsonValue(QJsonArray::fromStringList(BookMeta::splitTags(value)))
                                                             : QJsonValue(value));
    meta.applyJson(json);
    return library.setMeta(id, meta, nullptr, later);
}

[[nodiscard]] QString said(const SyncSummary &summary) {
    return (QStringList{summary.error} + summary.problems + summary.unannounced).join('\n');
}

} // namespace

class DetailClearsTests : public QObject {
    Q_OBJECT

private slots:
    void aChangeOrAClearReachesTheOtherComputer_data() {
        QTest::addColumn<bool>("paper");
        QTest::addColumn<bool>("both");
        QTest::addColumn<QString>("field");
        QTest::addColumn<QString>("value");
        QTest::addColumn<bool>("here");
        const QList<std::pair<QString, QString>> fields{
            {"title", "Clanbook: Ventrue (Revised)"},
            {"authors", "Bea Author"},
            {"isbn", "9781565041950"},
            {"series", "Clanbooks Revised"},
            {"description", "Lords of the night."},
            {"publisher", "Onyx Path"},
            {"tags", "Horror"},
        };
        // Of each kind of book, made on both computers or synced from here to there.
        const QList<std::pair<bool, bool>> kinds{{true, false}, {true, true}, {false, false}, {false, true}};
        // Each field changed or cleared (but the title: a book always has one), here or there.
        const QList<std::pair<bool, bool>> ways{{false, true}, {false, false}, {true, true}, {true, false}};
        for (const auto &[paper, both] : kinds)
            for (const auto &[field, value] : fields)
                for (const auto &[clear, here] : ways)
                    if (!clear || field != "title")
                        QTest::addRow("%s %s, %s %s %s", paper ? "on paper" : "a file", both ? "made on both" : "synced", qPrintable(field),
                                      clear ? "cleared" : "changed", here ? "here" : "there")
                            << paper << both << field << (clear ? QString() : value) << here;
    }

    void aChangeOrAClearReachesTheOtherComputer() {
        QFETCH(const bool, paper);
        QFETCH(const bool, both);
        QFETCH(const QString, field);
        QFETCH(const QString, value);
        QFETCH(const bool, here);
        QTemporaryDir dir;
        const QString rootB = dir.filePath("B");
        const QString ssh = Fixtures::standInSsh(dir.path(), OMABOOK_BINARY);
        Library a(dir.filePath("A"));
        Library b(rootB);
        QVERIFY(a.open() && b.open());
        const qint64 mine = made(a, paper, dir.path());
        QVERIFY(mine > 0);
        QVERIFY(!both || made(b, paper, dir.path()) > 0);
        const Remote toB("laptop:" + rootB, {ssh});
        const SyncSummary first = syncLibrary(a, toB);
        QVERIFY2(first.error.isEmpty() && first.problems.isEmpty() && first.unannounced.isEmpty(), qPrintable(said(first)));
        QCOMPARE(b.books().size(), 1);
        const qint64 theirs = b.books().first().id;
        QCOMPARE(fieldOf(b, theirs, field), fieldOf(a, mine, field));

        Library &changed = here ? a : b;
        const qint64 id = here ? mine : theirs;
        const QJsonValue before = fieldOf(changed, id, field);
        QVERIFY(change(changed, id, field, value));
        const QJsonValue after = fieldOf(changed, id, field);
        QVERIFY(after != before);

        SyncOptions options;
        if (value.isEmpty()) {
            // A clear is refused until it is accepted for the book.
            const SyncSummary refused = syncLibrary(a, toB);
            QVERIFY2(refused.error.contains(title), qPrintable(said(refused)));
            QCOMPARE(fieldOf(here ? b : a, here ? theirs : mine, field), before);
            options.accept = {title};
        }
        const SyncSummary synced = syncLibrary(a, toB, {}, options);
        QVERIFY2(synced.error.isEmpty() && synced.problems.isEmpty() && synced.unannounced.isEmpty(), qPrintable(said(synced)));
        QCOMPARE(fieldOf(a, mine, field), after);
        QCOMPARE(fieldOf(b, theirs, field), after);
    }

    // A book that a computer got by a sync goes on to a third whole, and so
    // does a change made later where it came from: A has it, B syncs with A
    // and C with B, each the one that asks or the one asked.
    void aBookGoesOnToAThirdComputer_data() {
        QTest::addColumn<bool>("paper");
        QTest::addColumn<bool>("bAsks");
        QTest::addColumn<bool>("cAsks");
        for (const bool paper : {true, false})
            for (const auto &[bAsks, cAsks] :
                 {std::pair{true, true}, std::pair{true, false}, std::pair{false, true}, std::pair{false, false}})
                QTest::addRow("%s, B %s, C %s", paper ? "on paper" : "a file", bAsks ? "asks" : "is asked", cAsks ? "asks" : "is asked")
                    << paper << bAsks << cAsks;
    }

    void aBookGoesOnToAThirdComputer() {
        QFETCH(const bool, paper);
        QFETCH(const bool, bAsks);
        QFETCH(const bool, cAsks);
        QTemporaryDir dir;
        const QString ssh = Fixtures::standInSsh(dir.path(), OMABOOK_BINARY);
        Library a(dir.filePath("A"));
        Library b(dir.filePath("B"));
        Library c(dir.filePath("C"));
        QVERIFY(a.open() && b.open() && c.open());
        const qint64 mine = made(a, paper, dir.path());
        QVERIFY(mine > 0);
        // From the one that has it to the next, the next asking or asked.
        const auto onTo = [&](Library &from, Library &to, bool asks) {
            return asks ? syncLibrary(to, Remote("laptop:" + from.root(), {ssh})) : syncLibrary(from, Remote("laptop:" + to.root(), {ssh}));
        };
        const auto arrives = [&] {
            for (const auto &[from, to, asks] : {std::tuple{&a, &b, bAsks}, std::tuple{&b, &c, cAsks}}) {
                const SyncSummary synced = onTo(*from, *to, asks);
                QVERIFY2(synced.error.isEmpty() && synced.problems.isEmpty() && synced.unannounced.isEmpty(), qPrintable(said(synced)));
                QCOMPARE(to->books().size(), 1);
                const SyncPlan::State there = SyncPlan::stateOf(*to, to->books().first());
                const SyncPlan::State here = SyncPlan::stateOf(a, *a.book(mine));
                for (const char *field : {"title", "authors", "isbn", "series", "description", "publisher", "tags", "identifiers", "paper"})
                    QCOMPARE(there.value(QLatin1StringView(field)), here.value(QLatin1StringView(field)));
            }
        };
        arrives();
        if (QTest::currentTestFailed())
            return;
        // Later, on A, a new description: it goes on to C too.
        QVERIFY(change(a, mine, "description", "Lords of the night."));
        arrives();
    }
};

QTEST_GUILESS_MAIN(DetailClearsTests)
#include "detailclears_tests.moc"
