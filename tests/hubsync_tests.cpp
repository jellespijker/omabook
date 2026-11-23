// A computer's sync with its hub (#188, ADR 0011): the hub is a peer, and a
// sync with it is the one two computers do, said through the computer's
// session (HubSession) to the hub's daemon (HubSync), which writes only
// through its keeper (ADR 0013), a moment snapshot first. Two computers and a
// hub end with the same libraries in whatever order they sync, and the same
// as two computers that sync with each other without it; a dry run changes
// nothing on either side; a library of another format is refused, saying
// which Omabook to update; and a sync is the key's computer's alone, reaching
// nothing on the hub but its own folders and the library's books.
//
// Everything here is in temporary folders: two libraries, and a hub with its
// keeper, its computers' keys and the session's pipe, all in this process.
// The same over a real sshd is in hublink_tests.

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <algorithm>
#include <memory>

#include "crypto.h"
#include "fixtures.h"
#include "hlc.h"
#include "hubcomputers.h"
#include "hubkeeper.h"
#include "hubremote.h"
#include "hubrequests.h"
#include "hubsession.h"
#include "hubsync.h"
#include "library.h"
#include "person.h"
#include "remote.h"
#include "spaces.h"
#include "sshkey.h"
#include "sync.h"

using Fixtures::whyNot;
using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QByteArray compact(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

// What a library holds, as both sides of a sync should end up holding it:
// each book by its global id, with what travels of it, sorted.
[[nodiscard]] QStringList contents(const Library &library) {
    QStringList books;
    for (const Book &book : library.books()) {
        QStringList hashes;
        for (const FormatFile &file : library.formatFiles(book.id))
            hashes << file.hash;
        hashes.sort();
        books << QStringList{Spaces::globalId(library, u"book"_s, book.id),
                             book.meta.title,
                             book.meta.authors.join(u" & "_s),
                             hashes.join(u","_s),
                             book.shelf,
                             QString::number(book.rating),
                             book.paper ? u"paper"_s : QString()}
                     .join(u" | "_s);
    }
    books.sort();
    return books;
}

// A person's computer: a library of its own.
struct Computer {
    QTemporaryDir dir;
    Library library{dir.filePath(u"library"_s)};

    Computer() { QVERIFY(library.open()); }

    // A book with a file, `title`, which is the same file on every computer that adds it.
    qint64 add(const QString &title) {
        const QString file = dir.filePath(title + u".epub"_s);
        if (!Fixtures::epub(file, title))
            return 0;
        return library.add(file).id;
    }
};

// A hub in a folder of its own: its keeper of one library ("home"), its
// requests with the sync among them, and its computers, laptop and desktop,
// linked by their keys. Each moment the keeper is told of is noted, with how
// many books the library had then.
struct Hub {
    QTemporaryDir dir;
    HubSync sync{dir.filePath(u"scratch"_s)};
    HubKeeper keeper{dir.filePath(u"libraries"_s)};
    HubRequests requests = HubRequests::usual();
    HubComputers computers{dir.filePath(u"state"_s), dir.filePath(u"home/.ssh/authorized_keys"_s), {u"/usr/bin/omabook"_s}};
    QList<std::pair<HubKeeper::Moment, qsizetype>> moments;
    QHash<QString, QString> fingerprints;

    [[nodiscard]] QString start() {
        QDir().mkpath(dir.filePath(u"scratch"_s));
        if (Library made(dir.filePath(u"libraries/home"_s)); !made.open())
            return u"no library made"_s;
        sync.addTo(requests);
        keeper.before([this](Library &library, HubKeeper::Moment moment) -> std::expected<void, Failure> {
            moments << std::pair{moment, library.books().size()};
            return {};
        });
        if (const auto opened = keeper.open(); !opened)
            return whyNot(opened);
        const auto person = Person::generate();
        if (!person)
            return person.error();
        for (const QString &name : {u"laptop"_s, u"desktop"_s}) {
            const auto key = SshKey::generate(dir.filePath(name), name);
            if (!key)
                return whyNot(key);
            const QString line = SshKey::line(*key);
            const Person::Signature signature = person->sign(Crypto::bytes(HubComputers::linkMessage(name, line)));
            const auto linked = computers.link({.name = name,
                                                .key = line,
                                                .identity = person->id(),
                                                .signature = QString::fromLatin1(Crypto::toByteArray(signature).toBase64(
                                                    QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals))});
            if (!linked)
                return whyNot(linked);
            fingerprints.insert(name, linked->fingerprint);
        }
        return {};
    }

    // A session as `computer`'s key opens it, its hello said, over the pipe
    // the daemon's socket would be.
    [[nodiscard]] std::unique_ptr<HubSession> session(const QString &computer) {
        auto opened = std::make_unique<HubSession>(computers, fingerprints.value(computer), [this](const QByteArray &request) {
            return requests.answerLine(request, keeper).trimmed();
        });
        std::ignore = opened->answer(compact(HubSession::hello())); // its greeting, which the tests of #180 read
        return opened;
    }

    // `computer`'s library synced with the hub's, through a session of its own.
    [[nodiscard]] SyncSummary syncWith(const QString &computer, Library &library, const SyncOptions &options = {}) {
        const auto opened = session(computer);
        return HubRemote::sync(library, u"hub"_s, [&opened](const QByteArray &line) { return opened->answer(line); }, {}, options);
    }

    // What the hub's library holds now.
    [[nodiscard]] QStringList held() {
        QStringList books;
        std::ignore = keeper.whole([&books](std::span<Library *const> libraries) -> std::expected<void, Failure> {
            books = contents(*libraries.front());
            return {};
        });
        return books;
    }

    [[nodiscard]] qsizetype syncMoments() const {
        return std::ranges::count_if(moments, [](const auto &moment) { return moment.first == HubKeeper::Moment::Sync; });
    }
};

// Every row of every table of a library's database, in a stable order (as #187's tests read it).
[[nodiscard]] QMap<QString, QStringList> rowsOf(const QString &file) {
    const QString connection = u"rows-"_s + QUuid::createUuid().toString(QUuid::Id128);
    QMap<QString, QStringList> tables;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(u"QSQLITE"_s, connection);
        database.setDatabaseName(file);
        database.setConnectOptions(u"QSQLITE_OPEN_READONLY"_s);
        if (database.open()) {
            QSqlQuery names(database);
            names.exec(u"SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name"_s);
            while (names.next())
                tables.insert(names.value(0).toString(), {});
            for (auto table = tables.begin(); table != tables.end(); ++table) {
                QSqlQuery rows(database);
                rows.exec(u"SELECT * FROM \""_s + table.key() + u"\""_s);
                while (rows.next()) {
                    QStringList values;
                    for (int column = 0; column < rows.record().count(); ++column)
                        values << QString::fromLatin1(rows.value(column).toByteArray().toBase64());
                    *table << values.join(u'|');
                }
                std::ranges::sort(*table);
            }
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return tables;
}

[[nodiscard]] QString problems(const SyncSummary &summary) { return summary.error + summary.problems.join(u'\n'); }

[[nodiscard]] QJsonObject asked(HubSession &session, const QJsonObject &request) {
    return QJsonDocument::fromJson(session.answer(compact(request))).object();
}

// Two computers' first books, each made the same on every computer.
void stockUp(Computer &laptop, Computer &desktop) {
    QVERIFY(laptop.add(u"Alpha"_s) > 0);
    QVERIFY(laptop.library.setReading(1, u"reading"_s, 4, {}));
    QVERIFY(desktop.add(u"Beta"_s) > 0);
    QVERIFY(desktop.add(u"Gamma"_s) > 0);
}

} // namespace

class HubSyncTests : public QObject {
    Q_OBJECT

private slots:
    void twoComputersSyncThroughTheirHub() {
        Hub hub;
        const QString started = hub.start();
        QVERIFY2(started.isEmpty(), qPrintable(started));
        Computer laptop;
        Computer desktop;
        stockUp(laptop, desktop);
        const SyncSummary first = hub.syncWith(u"laptop"_s, laptop.library);
        QVERIFY2(first.error.isEmpty() && first.problems.isEmpty(), qPrintable(problems(first)));
        QCOMPARE(first.sent, 1);
        QCOMPARE(hub.held(), contents(laptop.library));
        const SyncSummary second = hub.syncWith(u"desktop"_s, desktop.library);
        QVERIFY2(second.problems.isEmpty(), qPrintable(problems(second)));
        QCOMPARE(second.received, 1);
        QCOMPARE(second.sent, 2);
        QVERIFY2(hub.syncWith(u"laptop"_s, laptop.library).problems.isEmpty(), "laptop again");
        QCOMPARE(contents(laptop.library).size(), 3);
        QCOMPARE(contents(desktop.library).size(), 3);
        QCOMPARE(hub.held().size(), 3);
        // A change on one, through the hub to the other, as through a computer in its place.
        Computer middle;
        Computer here;
        Computer there;
        stockUp(here, there);
        const QString ssh = Fixtures::standInSsh(middle.dir.path(), QString::fromLatin1(OMABOOK_BINARY));
        const auto withMiddle = [&](Library &library) {
            return syncLibrary(library, Remote(u"middle:"_s + middle.dir.filePath(u"library"_s), {ssh})).problems.isEmpty();
        };
        QVERIFY(withMiddle(here.library) && withMiddle(there.library) && withMiddle(here.library));
        for (Library *library : {&desktop.library, &there.library})
            QVERIFY(library->setReading(library->books(u"Alpha"_s).value(0).id, u"read"_s, 2, u"2026-11-01"_s));
        QVERIFY(hub.syncWith(u"desktop"_s, desktop.library).problems.isEmpty());
        QVERIFY(hub.syncWith(u"laptop"_s, laptop.library).problems.isEmpty());
        QVERIFY(withMiddle(there.library) && withMiddle(here.library));
        QCOMPARE(contents(laptop.library), contents(here.library));
        QCOMPARE(contents(desktop.library), contents(there.library));
        QCOMPARE(hub.held(), contents(middle.library));
    }

    // The hub ends each order as a computer in its place does. A book that reaches a
    // computer through a third one, which was sent it, arrives there without its
    // details, hub or not: a bug of the sync itself, so the libraries are compared
    // with the computer in the hub's place, and only by their books with the pair.
    void everyOrderEndsAsAComputerToComputerSyncDoes() {
        // Without the hub: the two computers with each other, over (a stand-in for) ssh.
        QStringList direct;
        {
            Computer laptop;
            Computer desktop;
            stockUp(laptop, desktop);
            const QString ssh = Fixtures::standInSsh(laptop.dir.path(), QString::fromLatin1(OMABOOK_BINARY));
            const SyncSummary synced = syncLibrary(laptop.library, Remote(u"desktop:"_s + desktop.dir.filePath(u"library"_s), {ssh}));
            QVERIFY2(synced.problems.isEmpty() && synced.error.isEmpty(), qPrintable(problems(synced)));
            direct = contents(laptop.library);
            QCOMPARE(contents(desktop.library), direct);
            QCOMPARE(direct.size(), 3);
        }
        // Through the hub, each computer first and last in turn, and both twice.
        for (const QStringList &order :
             {QStringList{u"laptop"_s, u"desktop"_s, u"laptop"_s}, QStringList{u"desktop"_s, u"laptop"_s, u"desktop"_s},
              QStringList{u"laptop"_s, u"laptop"_s, u"desktop"_s, u"desktop"_s, u"laptop"_s}}) {
            // A computer in the hub's place, over ssh: what the hub is to do as a peer.
            Computer middle;
            Computer here;
            Computer there;
            stockUp(here, there);
            const QString ssh = Fixtures::standInSsh(middle.dir.path(), QString::fromLatin1(OMABOOK_BINARY));
            for (const QString &computer : order) {
                const SyncSummary synced = syncLibrary(computer == u"laptop"_s ? here.library : there.library,
                                                       Remote(u"middle:"_s + middle.dir.filePath(u"library"_s), {ssh}));
                QVERIFY2(synced.problems.isEmpty() && synced.error.isEmpty(), qPrintable(order.join(u' ') + u": "_s + problems(synced)));
            }
            Hub hub;
            const QString started = hub.start();
            QVERIFY2(started.isEmpty(), qPrintable(started));
            Computer laptop;
            Computer desktop;
            stockUp(laptop, desktop);
            for (const QString &computer : order) {
                const SyncSummary synced = hub.syncWith(computer, computer == u"laptop"_s ? laptop.library : desktop.library);
                QVERIFY2(synced.problems.isEmpty() && synced.error.isEmpty(), qPrintable(order.join(u' ') + u": "_s + problems(synced)));
            }
            QCOMPARE(contents(laptop.library), contents(here.library));
            QCOMPARE(contents(desktop.library), contents(there.library));
            QCOMPARE(hub.held(), contents(middle.library));
            // Both hold every book, as the two that sync with each other alone do.
            QCOMPARE(contents(laptop.library).size(), direct.size());
            QCOMPARE(hub.held().size(), direct.size());
        }
    }

    void aDryRunChangesNothingOnEitherSide() {
        Hub hub;
        const QString started = hub.start();
        QVERIFY2(started.isEmpty(), qPrintable(started));
        Computer laptop;
        Computer desktop;
        stockUp(laptop, desktop);
        QVERIFY(hub.syncWith(u"desktop"_s, desktop.library).problems.isEmpty());
        const QStringList there = hub.held();
        const QStringList here = contents(laptop.library);
        const qsizetype snapshots = hub.syncMoments();
        const SyncSummary dry = hub.syncWith(u"laptop"_s, laptop.library, [] {
            SyncOptions options;
            options.dryRun = true;
            return options;
        }());
        QVERIFY2(dry.error.isEmpty() && dry.problems.isEmpty(), qPrintable(problems(dry)));
        QVERIFY(dry.dryRun);
        QCOMPARE(dry.titles.sent, QStringList{u"Alpha"_s});
        QCOMPARE(dry.received, 2);
        QCOMPARE(hub.held(), there);
        QCOMPARE(contents(laptop.library), here);
        // No snapshot for what writes nothing.
        QCOMPARE(hub.syncMoments(), snapshots);
        // And a step that would write, asked in a dry run, is refused.
        const auto session = hub.session(u"laptop"_s);
        const QJsonObject begun =
            asked(*session, {{"request", "sync"}, {"step", "begin"}, {"format", laptop.library.formatVersion()}, {"dry_run", true}});
        QVERIFY2(begun.value("ok").toBool(), qPrintable(begun.value("error").toString()));
        const QJsonObject refused = asked(*session, {{"request", "sync"},
                                                     {"visit", begun.value("visit")},
                                                     {"step", "omabook"},
                                                     {"arguments", QJsonArray{u"set"_s, u"1"_s, u"--rating"_s, u"1"_s}}});
        QCOMPARE(refused.value("error").toString(), u"A dry run changes nothing on the hub."_s);
        QCOMPARE(hub.held(), there);
    }

    void theHubSnapshotsOnceBeforeASyncWrites() {
        Hub hub;
        const QString started = hub.start();
        QVERIFY2(started.isEmpty(), qPrintable(started));
        Computer laptop;
        Computer desktop;
        stockUp(laptop, desktop);
        hub.moments.clear();
        QVERIFY(hub.syncWith(u"laptop"_s, laptop.library).problems.isEmpty());
        // Once for the whole sync, and before its first book arrived.
        QCOMPARE(hub.moments.size(), 1);
        QCOMPARE(hub.moments.first().first, HubKeeper::Moment::Sync);
        QCOMPARE(hub.moments.first().second, 0);
        QCOMPARE(hub.held().size(), 1);
        // A snapshot that can't be taken refuses every write of the sync there: the hub is as it was.
        hub.keeper.before([](Library &, HubKeeper::Moment moment) -> std::expected<void, Failure> {
            if (moment == HubKeeper::Moment::Sync)
                return std::unexpected(Failure::said(u"no room for the snapshot"_s));
            return {};
        });
        const SyncSummary refused = hub.syncWith(u"desktop"_s, desktop.library);
        QVERIFY2(problems(refused).contains(u"no room for the snapshot"_s), qPrintable(problems(refused)));
        QCOMPARE(hub.held().size(), 1);
    }

    // The sync guard (#187) checks a hub sync as any other: a plan that would
    // empty a book's publisher, cleared later on the other side, is refused
    // before either side writes, whichever side it would happen on; the
    // computer's library and the hub's are as they were, and the hub took no
    // snapshot. Accepted for that book, it goes through, as announced.
    void aSyncTheGuardRefusesWritesNothingOnEitherSide_data() {
        QTest::addColumn<bool>("clearedOnHub");
        QTest::addColumn<QString>("side");
        QTest::newRow("cleared on the hub") << true << u"Kindred (here)"_s;
        QTest::newRow("cleared on the computer") << false << u"Kindred (on hub)"_s;
    }

    void aSyncTheGuardRefusesWritesNothingOnEitherSide() {
        QFETCH(bool, clearedOnHub);
        QFETCH(QString, side);
        Hub hub;
        const QString started = hub.start();
        QVERIFY2(started.isEmpty(), qPrintable(started));
        Computer laptop;
        const qint64 kindred = laptop.add(u"Kindred"_s);
        QVERIFY(kindred > 0);
        const QString publisher = laptop.library.book(kindred)->meta.publisher;
        QVERIFY(!publisher.isEmpty());
        // The hub has the same book of its own, as two computers each with it do in #187's test.
        qint64 held = 0;
        std::ignore = hub.keeper.whole([&](std::span<Library *const> libraries) -> std::expected<void, Failure> {
            held = libraries.front()->add(laptop.dir.filePath(u"Kindred.epub"_s)).id;
            return {};
        });
        QVERIFY(held > 0);
        const SyncSummary first = hub.syncWith(u"laptop"_s, laptop.library);
        QVERIFY2(first.error.isEmpty() && first.problems.isEmpty() && first.unannounced.isEmpty(),
                 qPrintable(problems(first) + first.unannounced.join(u'\n')));
        const auto clear = [](Library &library) {
            const Book book = library.books(u"Kindred"_s).value(0);
            BookMeta cleared = book.meta;
            cleared.publisher.clear();
            // A minute on, as a sync's details are timed to the second.
            return library.setMeta(book.id, cleared, nullptr, Hlc::pack(QDateTime::currentSecsSinceEpoch() + 60, 0));
        };
        bool cleared = false;
        if (clearedOnHub)
            std::ignore = hub.keeper.whole([&](std::span<Library *const> libraries) -> std::expected<void, Failure> {
                cleared = clear(*libraries.front());
                return {};
            });
        else
            cleared = clear(laptop.library);
        QVERIFY(cleared);
        hub.moments.clear();
        const QString hubDatabase = Library::databaseIn(hub.dir.filePath(u"libraries/home"_s));
        const QString laptopDatabase = Library::databaseIn(laptop.library.root());
        const auto hubRows = rowsOf(hubDatabase);
        const auto laptopRows = rowsOf(laptopDatabase);
        QVERIFY(!hubRows.isEmpty() && !laptopRows.isEmpty());
        const SyncSummary refused = hub.syncWith(u"laptop"_s, laptop.library);
        QVERIFY2(refused.error.contains(u"Kindred"_s) && refused.error.contains(publisher), qPrintable(refused.error));
        QVERIFY2(refused.breaches.join(u'\n').contains(side), qPrintable(refused.breaches.join(u'\n')));
        QCOMPARE(rowsOf(hubDatabase), hubRows);
        QCOMPARE(rowsOf(laptopDatabase), laptopRows);
        QCOMPARE(hub.syncMoments(), 0);
        SyncOptions accepted;
        accepted.accept = {u"kindred"_s};
        const SyncSummary through = hub.syncWith(u"laptop"_s, laptop.library, accepted);
        QVERIFY2(through.error.isEmpty() && through.problems.isEmpty() && through.unannounced.isEmpty(),
                 qPrintable(problems(through) + through.unannounced.join(u'\n')));
        QCOMPARE(through.breaches, refused.breaches);
        QCOMPARE(laptop.library.book(kindred)->meta.publisher, QString());
        QCOMPARE(hub.syncMoments(), 1);
    }

    void aLibraryOfAnotherFormatIsRefused() {
        Hub hub;
        const QString started = hub.start();
        QVERIFY2(started.isEmpty(), qPrintable(started));
        Computer laptop;
        QVERIFY(laptop.add(u"Alpha"_s) > 0);
        const int format = laptop.library.formatVersion();
        const auto session = hub.session(u"laptop"_s);
        const auto begin = [&session](int theirs) { return asked(*session, {{"request", "sync"}, {"step", "begin"}, {"format", theirs}}); };
        const QJsonObject newer = begin(format + 1);
        QVERIFY(!newer.value("ok").toBool());
        QCOMPARE(
            newer.value("error").toString(),
            u"The hub's library home is in an older format (%1) than this computer's (%2): update Omabook on the hub, then sync again."_s
                .arg(format)
                .arg(format + 1));
        const QJsonObject older = begin(format - 1);
        QVERIFY2(older.value("error").toString().contains(
                     u"newer format (%1) than this computer's (%2): update Omabook on this computer"_s.arg(format).arg(format - 1)),
                 qPrintable(older.value("error").toString()));
        QVERIFY(!asked(*session, {{"request", "sync"}, {"step", "begin"}}).value("ok").toBool());
        // Nothing written, nothing snapshotted, and the hub says why in its log too.
        QCOMPARE(hub.syncMoments(), 0);
        QVERIFY(hub.held().isEmpty());
        QVERIFY(begin(format).value("ok").toBool());
    }

    void aSyncIsItsKeysComputersAndReachesOnlyItsOwn() {
        Hub hub;
        const QString started = hub.start();
        QVERIFY2(started.isEmpty(), qPrintable(started));
        Computer laptop;
        QVERIFY(laptop.add(u"Alpha"_s) > 0);
        QVERIFY(hub.syncWith(u"laptop"_s, laptop.library).problems.isEmpty());
        const auto session = hub.session(u"laptop"_s);
        const QJsonObject begun = asked(*session, {{"request", "sync"}, {"step", "begin"}, {"format", laptop.library.formatVersion()}});
        QVERIFY2(begun.value("ok").toBool(), qPrintable(begun.value("error").toString()));
        const QJsonValue visit = begun.value("visit");
        const auto step = [&visit](HubSession &by, QJsonObject request) {
            request.insert("request", "sync");
            request.insert("visit", visit);
            return asked(by, request);
        };
        // The desktop's key, saying it is the laptop: its session is the desktop's, and the visit isn't.
        const auto desktop = hub.session(u"desktop"_s);
        const QJsonObject claimed =
            step(*desktop, {{"step", "omabook"}, {"computer", "laptop"}, {"arguments", QJsonArray{u"manifest"_s, u"--log"_s}}});
        QCOMPARE(claimed.value("error").toString(), u"No sync of this computer's is under way on the hub: begin it again."_s);
        QVERIFY(step(*session, {{"step", "omabook"}, {"arguments", QJsonArray{u"manifest"_s, u"--log"_s}}}).value("ok").toBool());
        // As the sync guard asks first (#187, #211).
        QVERIFY(step(*session, {{"step", "omabook"}, {"arguments", QJsonArray{u"manifest"_s, u"--state"_s}}}).value("ok").toBool());
        // Only the commands a sync runs there.
        for (const QJsonArray &arguments :
             {QJsonArray{u"remove"_s, u"1"_s}, QJsonArray{u"hub"_s, u"forget"_s, u"desktop"_s}, QJsonArray{u"manifest"_s},
              QJsonArray{u"sync"_s, u"--take-plugin-files"_s, u"/etc/passwd"_s}, QJsonArray{u"add"_s, u"/etc/passwd"_s, u"--json"_s},
              QJsonArray{u"set"_s, u"1"_s, u"--cover=/etc/passwd"_s}, QJsonArray{u"set"_s, u"1"_s, u"--cover"_s, u"../../x.jpg"_s}}) {
            const QJsonObject refused = step(*session, {{"step", "omabook"}, {"arguments", arguments}});
            QVERIFY2(!refused.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(arguments).toJson())));
        }
        QCOMPARE(hub.held().size(), 1);
        // Files only into its own folders, and only its books' and their covers' out.
        const QString folder = step(*session, {{"step", "folder"}}).value("folder").toString();
        QVERIFY(!folder.isEmpty());
        const QString data = QString::fromLatin1(QByteArray("pwned").toBase64());
        for (const QString &outside : {u"../x"_s, u"/tmp/x"_s, folder + u"/../../x"_s, u"x"_s})
            QVERIFY2(!step(*session, {{"step", "put"}, {"file", outside}, {"at", 0}, {"data", data}}).value("ok").toBool(),
                     qPrintable(outside));
        QVERIFY(step(*session, {{"step", "put"}, {"file", folder + u"/x"_s}, {"at", 0}, {"data", data}}).value("ok").toBool());
        QVERIFY(!QFileInfo::exists(hub.dir.filePath(u"x"_s)));
        QString book;
        std::ignore = hub.keeper.whole([&book](std::span<Library *const> libraries) -> std::expected<void, Failure> {
            book = libraries.front()->formatFiles(1).value(0).path;
            return {};
        });
        QVERIFY(step(*session, {{"step", "fetch"}, {"file", book}, {"at", 0}}).value("ok").toBool());
        for (const QString &other :
             {Library::databaseIn(hub.dir.filePath(u"libraries/home"_s)), hub.dir.filePath(u"state/computers.json"_s), u"/etc/passwd"_s,
              QFileInfo(book).absolutePath() + u"/../../library.db"_s}) {
            const QJsonObject refused = step(*session, {{"step", "fetch"}, {"file", other}, {"at", 0}});
            QVERIFY2(refused.value("error").toString().startsWith(u"In a sync, the hub gives only its books"_s), qPrintable(other));
        }
        // Over, and gone with what it was sent.
        QVERIFY(step(*session, {{"step", "end"}}).value("ok").toBool());
        QVERIFY(!step(*session, {{"step", "folder"}}).value("ok").toBool());
        QVERIFY(QDir(hub.dir.filePath(u"scratch"_s)).isEmpty());
        // Asked by no computer's key (the hub's own `omabook hub session`), there is no sync.
        QCOMPARE(hub.requests.answer({{"request", "sync"}, {"step", "begin"}, {"format", 1}}, hub.keeper).value("error").toString(),
                 u"Only a computer linked to the hub syncs with it."_s);
    }

    // #211: an Omabook from before the sync guard (#187) checks no plan, so it
    // never asks the books' state the guard compares first: a hub refuses a
    // step that would write in such a computer's sync, before writing anything
    // or taking a snapshot, saying to update it. Asked first, it goes through.
    void aComputerWhoseOmabookHasNoGuardIsRefused() {
        Hub hub;
        const QString started = hub.start();
        QVERIFY2(started.isEmpty(), qPrintable(started));
        Computer laptop;
        QVERIFY(laptop.add(u"Alpha"_s) > 0);
        QVERIFY(hub.syncWith(u"laptop"_s, laptop.library).problems.isEmpty());
        const QStringList held = hub.held();
        const auto session = hub.session(u"laptop"_s);
        const QJsonObject begun = asked(*session, {{"request", "sync"}, {"step", "begin"}, {"format", laptop.library.formatVersion()}});
        QVERIFY2(begun.value("ok").toBool(), qPrintable(begun.value("error").toString()));
        const auto omabook = [&](const QJsonArray &arguments) {
            return asked(*session, {{"request", "sync"}, {"visit", begun.value("visit")}, {"step", "omabook"}, {"arguments", arguments}});
        };
        const QJsonArray rate{u"set"_s, u"1"_s, u"--rating"_s, u"4"_s};
        hub.moments.clear();
        const QJsonObject older = omabook(rate);
        QVERIFY(!older.value("ok").toBool());
        QCOMPARE(older.value("error").toString(),
                 u"Omabook on this computer is older than the sync guard, which checks a sync before it writes anything: "
                 "update Omabook on this computer, then sync again."_s);
        // The plugins' files' steps write too (#212): the same.
        QCOMPARE(omabook({u"sync"_s, u"--plugin-files"_s}).value("error").toString(), older.value("error").toString());
        QCOMPARE(hub.syncMoments(), 0);
        QCOMPARE(hub.held(), held);
        QVERIFY(omabook({u"manifest"_s, u"--state"_s}).value("ok").toBool());
        const QJsonObject current = omabook(rate);
        QVERIFY2(current.value("ok").toBool(), qPrintable(current.value("error").toString()));
        QCOMPARE(hub.syncMoments(), 1);
    }

    // #211: and a hub that sends no plan (an Omabook there from before #187's
    // hub sync) is refused by the computer, before either side writes, saying
    // to update it.
    void aHubThatSendsNoPlanIsRefused() {
        Hub hub;
        const QString started = hub.start();
        QVERIFY2(started.isEmpty(), qPrintable(started));
        Computer laptop;
        QVERIFY(laptop.add(u"Alpha"_s) > 0);
        const QString hubDatabase = Library::databaseIn(hub.dir.filePath(u"libraries/home"_s));
        const QString laptopDatabase = Library::databaseIn(laptop.library.root());
        const auto hubRows = rowsOf(hubDatabase);
        const auto laptopRows = rowsOf(laptopDatabase);
        const auto opened = hub.session(u"laptop"_s);
        // What that hub answers: this one's, with no plan.
        const auto older = [&opened](const QByteArray &line) {
            QJsonObject answer = QJsonDocument::fromJson(opened->answer(line)).object();
            QJsonObject said = answer.value("answer").toObject();
            if (said.contains("plan")) {
                said.remove("plan");
                answer.insert("answer", said);
            }
            return compact(answer);
        };
        const SyncSummary refused = HubRemote::sync(laptop.library, u"hub"_s, older);
        QVERIFY2(refused.error.contains(u"update Omabook on the hub"_s), qPrintable(problems(refused)));
        QVERIFY(refused.uncheckedThere);
        QCOMPARE(refused.sent, 0);
        QCOMPARE(rowsOf(hubDatabase), hubRows);
        QCOMPARE(rowsOf(laptopDatabase), laptopRows);
        QCOMPARE(hub.syncMoments(), 0);
    }
};

QTEST_GUILESS_MAIN(HubSyncTests)
#include "hubsync_tests.moc"
