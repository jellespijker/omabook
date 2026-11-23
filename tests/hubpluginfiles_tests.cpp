// Plugins' files through a hub (#212): what a sync between two computers
// carries of the plugins' data/ folders (PluginFileSync, #131), a hub sync
// carries too, as it carries the library (#188): through the computer's
// session (HubSession) to the hub's daemon (HubSync), written there only
// through its keeper and inside the sync's one moment snapshot, kept in the
// library's own folder whether a plugin is installed on the hub or not, and
// checked as the guard checks a book (#187): no file is lost, and none
// emptied. A notebook is private, the person's own (#127): its recording goes
// only to one's own computers, the hub of the same person among them, and to
// no library that isn't.
//
// Everything here is in temporary folders: two computers with their plugins,
// and a hub with its keeper, its computers' keys and the session's pipe, all
// in this process (as in hubsync_tests).

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <algorithm>
#include <memory>

#include "crypto.h"
#include "failure.h"
#include "fixtures.h"
#include "hubcomputers.h"
#include "hubkeeper.h"
#include "hubremote.h"
#include "hubrequests.h"
#include "hubsession.h"
#include "hubsync.h"
#include "hubsyncsteps.h"
#include "library.h"
#include "person.h"
#include "plugindata.h"
#include "pluginfiles.h"
#include "pluginfilesync.h"
#include "privatebooks.h"
#include "remote.h"
#include "spaces.h"
#include "sshkey.h"
#include "sync.h"

using Fixtures::whyNot;
using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QByteArray compact(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

[[nodiscard]] QByteArray contents(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool put(const QString &path, const QByteArray &bytes) {
    return QDir().mkpath(QFileInfo(path).absolutePath()) && Fixtures::write(path, bytes);
}

// A recording of a few seconds: larger than one step of a hub sync carries (HubSync::piece).
[[nodiscard]] QByteArray recording(char tone) {
    QByteArray bytes = "OggS";
    bytes.append(HubSync::piece + 4096, tone);
    return bytes;
}

// A person's computer: a library and its plugins, Notebooks and Storyteller
// installed as they are (Notebooks keeps its notebooks private).
struct Computer {
    QTemporaryDir dir;
    Library library{dir.filePath(u"library"_s)};
    QString plugins = dir.filePath(u"plugins"_s);

    Computer() {
        QVERIFY(library.open());
        install(u"notebooks"_s, u"Notebooks"_s, QJsonArray{u"omanotebook"_s});
        install(u"storyteller"_s, u"Storyteller"_s, {});
    }

    void install(const QString &id, const QString &name, const QJsonArray &kept) {
        QJsonObject manifest{{"name", name}, {"version", "1.0"}, {"script", "main.js"}, {"permissions", QJsonArray{u"files"_s}}};
        if (!kept.isEmpty())
            manifest.insert("private", kept);
        QVERIFY(QDir().mkpath(plugins + u'/' + id + u"/data"_s));
        QVERIFY(Fixtures::write(plugins + u'/' + id + u"/plugin.json"_s, QJsonDocument(manifest).toJson()));
        QVERIFY(Fixtures::write(plugins + u'/' + id + u"/main.js"_s, "var loaded = true;"));
    }

    [[nodiscard]] QString file(const QString &plugin, const QString &path) const { return plugins + u'/' + plugin + u"/data/"_s + path; }

    // A private notebook with a page and a recording made on it, as Notebooks keeps them:
    // the notebook and the recording in its space of the log, the sound in its data/ folder.
    void notebookWithARecording(const QByteArray &sound) {
        QVERIFY(
            PrivateBooks::declare(library, {.format = u"omanotebook"_s, .plugin = u"notebooks"_s, .title = u"Notebooks"_s}).has_value());
        QVERIFY(PluginData::set(library, {u"notebooks"_s, u"notebook"_s, u"n1"_s}, {{"title", "Field notes"}, {"private", true}}));
        QVERIFY(PluginData::set(library, {u"notebooks"_s, u"page"_s, u"p1"_s}, {{"notebook", "n1"}, {"order", 1}}));
        QVERIFY(PluginData::set(library, {u"notebooks"_s, u"recording"_s, u"r1"_s},
                                {{"page", "p1"}, {"file", "data/recordings/r1.opus"}, {"duration", 4000}}));
        QVERIFY(put(file(u"notebooks"_s, u"recordings/r1.opus"_s), sound));
    }

    void story(const QByteArray &text) {
        QVERIFY(put(file(u"storyteller"_s, u"stories/s1.story"_s), R"({"title": "The Long Way", "text": ")" + text + R"("})"));
    }

    [[nodiscard]] SyncOptions options() const {
        SyncOptions options;
        options.plugins = plugins;
        return options;
    }
};

// A hub with its keeper of one library ("home"), its requests with the sync
// among them, and its computers, laptop and desktop, linked by their keys.
// What its story held at each moment snapshot is noted.
struct Hub {
    QTemporaryDir dir;
    HubSync sync{dir.filePath(u"scratch"_s)};
    HubKeeper keeper{dir.filePath(u"libraries"_s)};
    HubRequests requests = HubRequests::usual();
    HubComputers computers{dir.filePath(u"state"_s), dir.filePath(u"home/.ssh/authorized_keys"_s), {u"/usr/bin/omabook"_s}};
    QHash<QString, QString> fingerprints;
    QList<std::pair<HubKeeper::Moment, QByteArray>> moments;

    [[nodiscard]] QString start() {
        QDir().mkpath(dir.filePath(u"scratch"_s));
        if (Library made(dir.filePath(u"libraries/home"_s)); !made.open())
            return u"no library made"_s;
        sync.addTo(requests);
        keeper.before([this](Library &, HubKeeper::Moment moment) -> std::expected<void, Failure> {
            moments << std::pair{moment, contents(file(u"storyteller"_s, u"stories/s1.story"_s))};
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

    [[nodiscard]] std::unique_ptr<HubSession> session(const QString &computer) {
        auto opened = std::make_unique<HubSession>(computers, fingerprints.value(computer), [this](const QByteArray &request) {
            return requests.answerLine(request, keeper).trimmed();
        });
        std::ignore = opened->answer(compact(HubSession::hello()));
        return opened;
    }

    [[nodiscard]] SyncSummary syncWith(const QString &name, Computer &computer, SyncOptions options = {}) {
        options.plugins = computer.plugins;
        const auto opened = session(name);
        return HubRemote::sync(computer.library, u"hub"_s, [&opened](const QByteArray &line) { return opened->answer(line); }, {}, options);
    }

    // Where the hub keeps a plugin's file: in its library's folder.
    [[nodiscard]] QString file(const QString &plugin, const QString &path) const {
        return HubSyncSteps::pluginsOf(dir.filePath(u"libraries/home"_s)) + u'/' + plugin + u"/data/"_s + path;
    }

    // What the hub's library says of a plugin's thing.
    [[nodiscard]] QJsonValue read(const PluginData::Thing &thing) {
        QJsonValue read;
        std::ignore = keeper.whole([&](std::span<Library *const> libraries) -> std::expected<void, Failure> {
            read = PluginData::read(*libraries.front(), thing);
            return {};
        });
        return read;
    }

    [[nodiscard]] qsizetype syncMoments() const {
        return std::ranges::count_if(moments, [](const auto &moment) { return moment.first == HubKeeper::Moment::Sync; });
    }
};

// A sync that pairs the library with the hub's (#214): another computer's, the first time.
[[nodiscard]] SyncOptions pairing() {
    SyncOptions options;
    options.pair = true;
    return options;
}

[[nodiscard]] QString problems(const SyncSummary &summary) {
    return summary.error + summary.problems.join(u'\n') + summary.unannounced.join(u'\n');
}

[[nodiscard]] bool clean(const SyncSummary &summary) {
    if (!summary.error.isEmpty() || !summary.problems.isEmpty() || !summary.unannounced.isEmpty())
        qWarning().noquote() << problems(summary);
    return summary.error.isEmpty() && summary.problems.isEmpty() && summary.unannounced.isEmpty();
}

[[nodiscard]] QJsonObject asked(HubSession &session, const QJsonObject &request) {
    return QJsonDocument::fromJson(session.answer(compact(request))).object();
}

} // namespace

class HubPluginFilesTests : public QObject {
    Q_OBJECT

private slots:
    // A notebook with its recording, and a story, made on the laptop reach the
    // desktop through the hub, which keeps them in its library's folder though
    // neither plugin is installed there; a change goes on again, a removal
    // removes them, and a plugin the desktop lacks leaves its files on the hub.
    void notebooksAndStoriesTravelThroughTheHub() {
        Hub hub;
        const QString started = hub.start();
        QVERIFY2(started.isEmpty(), qPrintable(started));
        Computer laptop;
        Computer desktop;
        const QByteArray sound = recording('a');
        laptop.notebookWithARecording(sound);
        laptop.story("Once.");
        laptop.install(u"sketches"_s, u"Sketches"_s, {});
        QVERIFY(put(laptop.file(u"sketches"_s, u"one.svg"_s), "<svg/>"));
        QVERIFY(put(laptop.file(u"notebooks"_s, u"settings.json"_s), R"({"here": true})"));

        const SyncSummary first = hub.syncWith(u"laptop"_s, laptop);
        QVERIFY(clean(first));
        QCOMPARE(first.pluginFilesSent, 3);
        QCOMPARE(contents(hub.file(u"notebooks"_s, u"recordings/r1.opus"_s)), sound);
        QCOMPARE(contents(hub.file(u"sketches"_s, u"one.svg"_s)), QByteArray("<svg/>"));
        QVERIFY(!QFileInfo::exists(hub.file(u"notebooks"_s, u"settings.json"_s)));

        const SyncSummary second = hub.syncWith(u"desktop"_s, desktop, pairing());
        QVERIFY(clean(second));
        QCOMPARE(second.pluginFilesTaken, 2);
        QCOMPARE(contents(desktop.file(u"notebooks"_s, u"recordings/r1.opus"_s)), sound);
        QCOMPARE(contents(desktop.file(u"storyteller"_s, u"stories/s1.story"_s)),
                 contents(laptop.file(u"storyteller"_s, u"stories/s1.story"_s)));
        QVERIFY(!QFileInfo::exists(desktop.plugins + u"/sketches"_s));
        // The notebook came along, private as it was, and stays out of every space but the plugin's.
        const QJsonValue notebook = PluginData::read(desktop.library, {u"notebooks"_s, u"notebook"_s, u"n1"_s});
        QCOMPARE(notebook["fields"]["private"].toBool(), true);
        QCOMPARE(hub.read({u"notebooks"_s, u"recording"_s, u"r1"_s})["fields"]["file"].toString(), u"data/recordings/r1.opus"_s);

        // Once: a sync after it carries nothing.
        const SyncSummary again = hub.syncWith(u"desktop"_s, desktop);
        QCOMPARE(again.pluginFilesTaken + again.pluginFilesSent, 0);

        // A chapter more on the desktop goes to the laptop through the hub.
        desktop.story("Once, and again.");
        QVERIFY(clean(hub.syncWith(u"desktop"_s, desktop)));
        const SyncSummary told = hub.syncWith(u"laptop"_s, laptop);
        QVERIFY(clean(told));
        QCOMPARE(told.pluginFilesTaken, 1);
        QVERIFY(contents(laptop.file(u"storyteller"_s, u"stories/s1.story"_s)).contains("and again"));

        // The recording removed on the laptop, as Notebooks removes it, goes from the hub and the desktop.
        const QString sounded = laptop.file(u"notebooks"_s, u"recordings/r1.opus"_s);
        QVERIFY(PluginFiles::forget(laptop.library, u"notebooks"_s, u"recordings/r1.opus"_s, Library::hashFile(sounded)));
        QVERIFY(QFile::remove(sounded));
        QVERIFY(clean(hub.syncWith(u"laptop"_s, laptop)));
        QVERIFY(!QFileInfo::exists(hub.file(u"notebooks"_s, u"recordings/r1.opus"_s)));
        QVERIFY(clean(hub.syncWith(u"desktop"_s, desktop)));
        QVERIFY(!QFileInfo::exists(desktop.file(u"notebooks"_s, u"recordings/r1.opus"_s)));
        QVERIFY(!QFileInfo::exists(sounded));
        QVERIFY(QFileInfo::exists(hub.file(u"sketches"_s, u"one.svg"_s)));
    }

    // The hub writes them only through its keeper, after the one moment
    // snapshot of the sync: the snapshot sees the story as it was before.
    void theHubsMomentSnapshotHoldsThemAsTheyWere() {
        Hub hub;
        const QString started = hub.start();
        QVERIFY2(started.isEmpty(), qPrintable(started));
        Computer laptop;
        laptop.story("Once.");
        QVERIFY(clean(hub.syncWith(u"laptop"_s, laptop)));
        const QByteArray before = contents(hub.file(u"storyteller"_s, u"stories/s1.story"_s));
        QVERIFY(!before.isEmpty());
        laptop.story("Once, and again.");
        hub.moments.clear();
        QVERIFY(clean(hub.syncWith(u"laptop"_s, laptop)));
        QCOMPARE(hub.syncMoments(), 1);
        QCOMPARE(hub.moments.first().second, before);
        QVERIFY(contents(hub.file(u"storyteller"_s, u"stories/s1.story"_s)).contains("and again"));
        // A snapshot that can't be taken leaves the hub's files as they were.
        hub.keeper.before([](Library &, HubKeeper::Moment moment) -> std::expected<void, Failure> {
            if (moment == HubKeeper::Moment::Sync)
                return std::unexpected(Failure::said(u"no room for the snapshot"_s));
            return {};
        });
        const QByteArray written = contents(hub.file(u"storyteller"_s, u"stories/s1.story"_s));
        laptop.story("Once, again, and a third time.");
        const SyncSummary refused = hub.syncWith(u"laptop"_s, laptop);
        QVERIFY2(problems(refused).contains(u"no room for the snapshot"_s), qPrintable(problems(refused)));
        QCOMPARE(contents(hub.file(u"storyteller"_s, u"stories/s1.story"_s)), written);
    }

    // The guard's promise for files: none is lost, and none emptied. A story
    // emptied on the desktop doesn't empty the hub's or the laptop's, which
    // says so, and the whole one comes back to the desktop; a file gone from
    // the hub without the plugin removing it comes back there, and goes from
    // no computer.
    void noFileIsLostOrEmptiedThroughTheHub() {
        Hub hub;
        const QString started = hub.start();
        QVERIFY2(started.isEmpty(), qPrintable(started));
        Computer laptop;
        Computer desktop;
        const QByteArray sound = recording('b');
        laptop.notebookWithARecording(sound);
        laptop.story("Once.");
        QVERIFY(clean(hub.syncWith(u"laptop"_s, laptop)));
        QVERIFY(clean(hub.syncWith(u"desktop"_s, desktop, pairing())));
        const QByteArray whole = contents(laptop.file(u"storyteller"_s, u"stories/s1.story"_s));
        QCOMPARE(contents(desktop.file(u"storyteller"_s, u"stories/s1.story"_s)), whole);

        QVERIFY(put(desktop.file(u"storyteller"_s, u"stories/s1.story"_s), {}));
        const SyncSummary emptied = hub.syncWith(u"desktop"_s, desktop);
        QVERIFY2(emptied.problems.join(u'\n').contains(u"stories/s1.story"_s), qPrintable(problems(emptied)));
        QCOMPARE(contents(hub.file(u"storyteller"_s, u"stories/s1.story"_s)), whole);
        std::ignore = hub.syncWith(u"laptop"_s, laptop);
        QCOMPARE(contents(laptop.file(u"storyteller"_s, u"stories/s1.story"_s)), whole);
        QVERIFY(clean(hub.syncWith(u"laptop"_s, laptop)));
        std::ignore = hub.syncWith(u"desktop"_s, desktop);
        QCOMPARE(contents(desktop.file(u"storyteller"_s, u"stories/s1.story"_s)), whole);

        QVERIFY(QFile::remove(hub.file(u"notebooks"_s, u"recordings/r1.opus"_s)));
        QVERIFY(clean(hub.syncWith(u"desktop"_s, desktop)));
        QCOMPARE(contents(hub.file(u"notebooks"_s, u"recordings/r1.opus"_s)), sound);
        QCOMPARE(contents(desktop.file(u"notebooks"_s, u"recordings/r1.opus"_s)), sound);
        QVERIFY(clean(hub.syncWith(u"laptop"_s, laptop)));
        QCOMPARE(contents(laptop.file(u"notebooks"_s, u"recordings/r1.opus"_s)), sound);
    }

    void aDryRunCarriesNoFiles() {
        Hub hub;
        const QString started = hub.start();
        QVERIFY2(started.isEmpty(), qPrintable(started));
        Computer laptop;
        laptop.notebookWithARecording(recording('c'));
        SyncOptions dry;
        dry.dryRun = true;
        const SyncSummary planned = hub.syncWith(u"laptop"_s, laptop, dry);
        QVERIFY2(planned.error.isEmpty() && planned.problems.isEmpty(), qPrintable(problems(planned)));
        QVERIFY(!QFileInfo::exists(HubSyncSteps::pluginsOf(hub.dir.filePath(u"libraries/home"_s))));
        QCOMPARE(hub.syncMoments(), 0);
        const auto session = hub.session(u"laptop"_s);
        const QJsonObject begun =
            asked(*session, {{"request", "sync"}, {"step", "begin"}, {"format", laptop.library.formatVersion()}, {"dry_run", true}});
        QVERIFY2(begun.value("ok").toBool(), qPrintable(begun.value("error").toString()));
        const QJsonObject refused = asked(*session, {{"request", "sync"},
                                                     {"visit", begun.value("visit")},
                                                     {"step", "omabook"},
                                                     {"arguments", QJsonArray{u"sync"_s, u"--plugin-files"_s}}});
        QCOMPARE(refused.value("error").toString(), u"A dry run changes nothing on the hub."_s);
    }

    // A private notebook's recording goes to one's own computers only: the hub
    // gives it, and takes one, only in a sync of a library it was paired with,
    // and a computer gives one to, and takes one from, no library that isn't
    // one of its own, hub or not.
    void aRecordingGoesOnlyToOnesOwnComputers() {
        Hub hub;
        const QString started = hub.start();
        QVERIFY2(started.isEmpty(), qPrintable(started));
        Computer laptop;
        laptop.notebookWithARecording(recording('d'));
        QVERIFY(clean(hub.syncWith(u"laptop"_s, laptop)));
        const QString kept = hub.file(u"notebooks"_s, u"recordings/r1.opus"_s);
        QVERIFY(QFileInfo::exists(kept));

        // The desktop's library, never synced yet, is none of the hub library's own (#214): its sync
        // is refused as it begins, so the hub's recording doesn't reach it, nor its own the hub.
        Computer desktop;
        desktop.story("Elsewhere.");
        const SyncSummary refused = hub.syncWith(u"desktop"_s, desktop);
        QVERIFY2(refused.error.contains(u"--pair"_s), qPrintable(problems(refused)));
        QCOMPARE(refused.pluginFilesSent + refused.pluginFilesTaken, 0);
        QVERIFY(!QFileInfo::exists(desktop.file(u"notebooks"_s, u"recordings/r1.opus"_s)));
        QVERIFY(!QFileInfo::exists(hub.file(u"storyteller"_s, u"stories/s1.story"_s)));
        // Over the session pipe: no visit, so no file is fetched, nor any taken.
        const auto session = hub.session(u"desktop"_s);
        const auto visit = [&](const QString &device) {
            return asked(*session,
                         {{"request", "sync"}, {"step", "begin"}, {"format", desktop.library.formatVersion()}, {"device", device}});
        };
        const auto step = [&](const QJsonValue &begun, QJsonObject request) {
            request.insert("request", "sync");
            request.insert("visit", begun);
            return asked(*session, request);
        };
        const QJsonObject stranger = visit(Spaces::deviceId(desktop.library));
        QVERIFY2(!stranger.value("ok").toBool() && stranger.value("error").toString().contains(u"--pair"_s),
                 qPrintable(QString::fromUtf8(compact(stranger))));
        QVERIFY(!step(stranger.value("visit"), {{"step", "fetch"}, {"file", kept}, {"at", 0}}).value("ok").toBool());
        QVERIFY(!step(stranger.value("visit"), {{"step", "folder"}}).value("ok").toBool());
        const QJsonObject took =
            step(stranger.value("visit"),
                 {{"step", "omabook"}, {"arguments", QJsonArray{u"sync"_s, u"--take-plugin-files"_s, u"f1/given.json"_s}}});
        QVERIFY2(!took.value("ok").toBool(), qPrintable(QString::fromUtf8(compact(took))));
        // Paired once asked, the files go both ways.
        const SyncSummary paired = hub.syncWith(u"desktop"_s, desktop, pairing());
        QVERIFY(clean(paired));
        QCOMPARE(contents(desktop.file(u"notebooks"_s, u"recordings/r1.opus"_s)), contents(kept));
        QVERIFY(contents(hub.file(u"storyteller"_s, u"stories/s1.story"_s)).contains("Elsewhere"));
        // The laptop's, paired at its sync, is.
        const QJsonValue own = visit(Spaces::deviceId(laptop.library)).value("visit");
        QVERIFY(step(own, {{"step", "fetch"}, {"file", kept}, {"at", 0}}).value("ok").toBool());

        // A library that isn't one of the laptop's own is given none and asked for none.
        PluginFileSync::Sides sides{.here = PluginFileSync::survey(laptop.library, laptop.plugins),
                                    .there = {{"folder", "/elsewhere"},
                                              {"plugins", QJsonObject{{"notebooks", QJsonObject{}}}},
                                              {"every", true},
                                              {"device", QUuid::createUuid().toString(QUuid::Id128)}}};
        SyncSummary summary;
        PluginFileSync::finish(laptop.library, Remote(u"elsewhere"_s, {u"/bin/false"_s}), laptop.plugins, sides, summary);
        QCOMPARE(summary.pluginFilesSent + summary.pluginFilesTaken, 0);
        QVERIFY2(summary.problems.join(u'\n').contains(u"your own computers"_s), qPrintable(summary.problems.join(u'\n')));
    }
};

QTEST_GUILESS_MAIN(HubPluginFilesTests)
#include "hubpluginfiles_tests.moc"
