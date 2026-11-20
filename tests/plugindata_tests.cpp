// A plugin's data in the change log (ADR 0002, #76): written with
// omabook.log.set, add and remove, of the plugin's own space, merged after a
// sync between one's own computers (both sides' additions kept, a removal
// going too), compacted like the rest, never the library's rows, never sent
// to an Omabook from before, and refused over its limits. A stand-in notebook
// keeps pages with a title and strokes.

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <memory>

#include "changelog.h"
#include "compaction.h"
#include "fixtures.h"
#include "library.h"
#include "plugindata.h"
#include "remote.h"
#include "replication.h"
#include "scriptplugin.h"
#include "scriptplugins.h"
#include "spacereplay.h"
#include "spaces.h"
#include "sync.h"
#include "synclog.h"

using namespace Qt::StringLiterals;

namespace {

using Fixtures::whyNot;

constexpr const char *notebook = R"(
function title(page, words) { omabook.log.set("page", page, {title: words}); return true; }
function draw(page, strokes, erased) { omabook.log.add("page", page, "strokes", strokes, erased); return true; }
function erase(page, keys) { omabook.log.remove("page", page, "strokes", keys); return true; }
function tear(page) { omabook.log.remove("page", page); return true; }
function page(id) { return omabook.log.read("page", id); }
function pages() { return omabook.log.list("page").map(function (one) { return one.id; }); }
function attempt(what) { try { what(); return ""; } catch (error) { return String(error.message); } }
function large() { return attempt(function () { omabook.log.set("page", "p", {ink: "x".repeat(70000)}); }); }
function many() {
    var strokes = {};
    for (var i = 0; i < 1001; ++i) strokes["s" + i] = i;
    return attempt(function () { omabook.log.add("page", "p", "strokes", strokes); });
}
function misnamed() { return attempt(function () { omabook.log.set("Page!", "p", {title: "x"}); }); }
function removedField() { return attempt(function () { omabook.log.set("page", "p", {removed: 1}); }); }
)";

void writePlugin(const QString &folder) {
    QVERIFY(QDir().mkpath(folder + "/notebook"));
    const QJsonObject manifest{{"name", "Notebook"}, {"version", "1.0"}, {"script", "main.js"}};
    QVERIFY(Fixtures::write(folder + "/notebook/plugin.json", QJsonDocument(manifest).toJson()));
    QVERIFY(Fixtures::write(folder + "/notebook/main.js", notebook));
}

QString ssh(const QTemporaryDir &dir) {
    const QByteArray standIn = Fixtures::standInSsh(dir.path(), OMABOOK_BINARY).toUtf8();
    return Fixtures::script(dir.filePath("ssh-current"), "exec '" + standIn + "' \"$@\"\n");
}

[[nodiscard]] PluginData::Thing page(const QString &id) { return {.plugin = u"notebook"_s, .entity = u"page"_s, .id = id}; }

[[nodiscard]] QStringList keys(const QJsonValue &thing, const QString &set = u"strokes"_s) {
    QStringList keys = thing.toObject().value("sets").toObject().value(set).toObject().keys();
    keys.sort();
    return keys;
}

[[nodiscard]] QString title(const QJsonValue &thing) { return thing.toObject().value("fields").toObject().value("title").toString(); }

[[nodiscard]] qint64 count(const Library &library, const QString &sql) {
    QSqlQuery query = ChangeLog::query(library, sql);
    return query.next() ? query.value(0).toLongLong() : -1;
}

// One computer with the notebook running on its library, and another computer's library beside it.
struct TwoComputers {
    TwoComputers() : folder(dir.filePath("plugins")), here(dir.filePath("Here")), there(dir.filePath("There")) {
        QVERIFY(here.open() && there.open());
        writePlugin(folder);
        ScriptServices services;
        services.library = [this] { return &here; };
        plugins = std::make_unique<ScriptPlugins>(services, folder, nullptr, false);
        QVERIFY(plugins->plugin("notebook") != nullptr);
    }

    QJsonValue call(const QString &function, const QJsonArray &arguments = {}) {
        QString problem;
        const QJsonValue answer = plugins->plugin("notebook")->callNow(function, arguments, &problem);
        [&] { QVERIFY2(problem.isEmpty(), qPrintable(problem)); }();
        return answer;
    }

    SyncSummary sync() { return syncLibrary(here, Remote("laptop:" + dir.filePath("There"), {ssh(dir)})); }

    QTemporaryDir dir;
    QString folder;
    Library here, there;
    std::unique_ptr<ScriptPlugins> plugins;
};

[[nodiscard]] SpaceOperation op(const QString &field, const QString &value, qint64 hlc, const QString &device = u"d1"_s) {
    return {.space = u"plugin:notebook"_s,
            .device = device,
            .seq = hlc,
            .hlc = hlc,
            .author = device,
            .entity = u"notebook:page"_s,
            .id = u"p1"_s,
            .field = field,
            .value = value};
}

} // namespace

class PluginDataTests : public QObject {
    Q_OBJECT

private slots:
    // What two computers write of a page merges at a sync: every stroke
    // either drew is kept, the title written later stays, and what one
    // erased or tore out goes on the other too. None of it is a book.
    void writtenOnTwoComputersItMergesAfterASync() {
        TwoComputers two;
        QVERIFY2(whyNot(PluginData::set(two.there, page("p1"), {{"title", "There"}})).isEmpty(), "set there");
        QVERIFY(PluginData::change(two.there, page("p1"), "strokes", {.added = {{"a", 1}, {"b", 2}}, .removed = {}}).has_value());
        QVERIFY(PluginData::set(two.there, page("p2"), {{"title", "Loose"}}).has_value());
        // Here's clock a minute on, so its title is the later one whatever the second.
        Spaces::setClockOffset(two.here, 60);
        QVERIFY(two.call("title", {"p1", "Here"}).toBool());
        QVERIFY(two.call("draw", {"p1", QJsonObject{{"c", 3}}}).toBool());

        SyncSummary summary = two.sync();
        QVERIFY2(summary.error.isEmpty() && summary.problems.isEmpty(), qPrintable(summary.error + summary.problems.join('\n')));
        QCOMPARE(keys(two.call("page", {"p1"})), (QStringList{"a", "b", "c"}));
        QCOMPARE(title(two.call("page", {"p1"})), u"Here"_s);
        QCOMPARE(two.call("pages").toArray(), (QJsonArray{"p1", "p2"}));
        QCOMPARE(keys(PluginData::read(two.there, page("p1"))), (QStringList{"a", "b", "c"}));
        QCOMPARE(title(PluginData::read(two.there, page("p1"))), u"Here"_s);

        // A stroke erased in part (one gone, its pieces added) and a page torn out.
        QVERIFY(two.call("draw", {"p1", QJsonObject{{"c1", 31}, {"c2", 32}}, QJsonArray{"c"}}).toBool());
        QVERIFY(two.call("erase", {"p1", QJsonArray{"a"}}).toBool());
        QVERIFY(two.call("tear", {"p2"}).toBool());
        summary = two.sync();
        QVERIFY2(summary.error.isEmpty() && summary.problems.isEmpty(), qPrintable(summary.error + summary.problems.join('\n')));
        QCOMPARE(keys(PluginData::read(two.there, page("p1"))), (QStringList{"b", "c1", "c2"}));
        QCOMPARE(PluginData::read(two.there, page("p2")), QJsonValue(QJsonValue::Null));
        QCOMPARE(PluginData::list(two.there, "notebook", "page").size(), 1);

        // Nothing of it reached the library's rows on either side, nor what they are made of.
        for (Library *library : {&two.here, &two.there}) {
            QVERIFY(library->books().isEmpty());
            const QList<SpaceOperation> rows = SpaceReplay::operations(*library);
            QVERIFY(std::ranges::none_of(rows, [](const SpaceOperation &one) { return PluginData::isPluginSpace(one.space); }));
            QCOMPARE(count(*library, "SELECT COUNT(*) FROM live_ops WHERE entity LIKE 'notebook:%'"), 0);
            QVERIFY(count(*library, "SELECT COUNT(*) FROM space_ops WHERE entity LIKE 'notebook:%'") > 0);
        }
        // Written again later, a page torn out comes back.
        QVERIFY(PluginData::set(two.there, page("p2"), {{"title", "Back"}}).has_value());
        QCOMPARE(title(PluginData::read(two.there, page("p2"))), u"Back"_s);
    }

    // An Omabook from before takes none of a plugin's data (its log reads
    // only what it knows), so it is never sent any: it would be sent all of
    // it at every sync. One that says it takes it is sent it.
    void anOmabookFromBeforeIsSentNone() {
        TwoComputers two;
        QVERIFY(two.call("draw", {"p1", QJsonObject{{"a", 1}}}).toBool());
        const QJsonObject before{
            {"space", "personal:older"}, {"device", "older"}, {"vector", QJsonObject()}, {"signs", 1}, {"dry_run", true}};
        const auto spaces = [](const QJsonObject &answer) {
            QStringList named;
            for (const QJsonValue &one : answer.value("ops").toArray())
                named << one.toArray().at(0).toString();
            return named;
        };
        QVERIFY(!spaces(SyncLog::answer(two.here, before)).contains("plugin:notebook"));
        QJsonObject now = before;
        now.insert("plugin_data", 1);
        QVERIFY(spaces(SyncLog::answer(two.here, now)).contains("plugin:notebook"));
    }

    // Compaction keeps of a field the latest and of a set every change, so
    // the page reads the same after it, with its older titles gone.
    void compactingKeepsWhatThePageSays() {
        TwoComputers two;
        for (const char *words : {"One", "Two", "Three"})
            QVERIFY(two.call("title", {"p1", words}).toBool());
        QVERIFY(two.call("draw", {"p1", QJsonObject{{"a", 1}, {"b", 2}}}).toBool());
        QVERIFY(two.call("draw", {"p1", QJsonObject{{"c", 3}}, QJsonArray{"a"}}).toBool());
        const QJsonValue before = two.call("page", {"p1"});
        QCOMPARE(keys(before), (QStringList{"b", "c"}));
        Compaction::compact(two.here);
        QCOMPARE(two.call("page", {"p1"}), before);
        QCOMPARE(count(two.here, "SELECT COUNT(*) FROM space_ops WHERE entity = 'notebook:page' AND field = 'title'"), 1);
        QCOMPARE(count(two.here, "SELECT COUNT(*) FROM space_ops WHERE entity = 'notebook:page' AND field = 'set:strokes'"), 2);
    }

    // What a script writes over the limits, or by a name that isn't one, is
    // thrown back at it and nothing of it is written.
    void aWriteOverTheLimitsThrows() {
        TwoComputers two;
        QVERIFY(two.call("large").toString().contains("65536"));
        QVERIFY(two.call("many").toString().contains("1000"));
        QVERIFY(two.call("misnamed").toString().contains("Page!"));
        QVERIFY(!two.call("removedField").toString().isEmpty());
        QCOMPARE(two.call("page", {"p"}), QJsonValue(QJsonValue::Null));
        QCOMPARE(count(two.here, "SELECT COUNT(*) FROM space_ops WHERE entity LIKE 'notebook:%'"), 0);
    }

    // What another computer sends is untrusted: of a plugin's space, an
    // operation over a limit, of another's entity, or that is no field or
    // change of a set, is refused and counted, even from one's own computer.
    void whatComesOverTheLimitsIsRefused() {
        QTemporaryDir dir;
        Library own(dir.filePath("Library"));
        QVERIFY(own.open());
        const QString device = Spaces::deviceId(own);
        QJsonObject strokes;
        for (int i = 0; i <= PluginData::maxItems; ++i)
            strokes.insert(u"s%1"_s.arg(i), i);
        QList<SpaceOperation> sent{
            op("title", "\"fine\"", 1, device),
            op("title", u"\"%1\""_s.arg(QString(PluginData::maxValueBytes, u'x')), 2, device),
            op("set:strokes", QString::fromUtf8(QJsonDocument(QJsonObject{{"add", strokes}}).toJson(QJsonDocument::Compact)), 3, device),
            op("set:strokes", "[1, 2]", 4, device),
            op("title", "not json", 5, device),
            op(QString(PluginData::maxNameLength + 1, u'a'), "1", 6, device),
        };
        SpaceOperation another = op("title", "\"theirs\"", 7, device);
        another.entity = u"other:page"_s;
        sent << another;
        SpaceOperation longId = op("title", "\"long\"", 8, device);
        longId.id = QString(PluginData::maxIdLength + 1, u'p');
        sent << longId;
        for (const SpaceOperation &one : std::as_const(sent).mid(1))
            QVERIFY2(!PluginData::whyRefused(one).isEmpty(), qPrintable(one.field.left(20) + one.entity + one.id.left(20)));
        const Replication::Admitted admitted = Replication::admit(own, sent, device);
        QCOMPARE(admitted.refused, static_cast<int>(sent.size()) - 1);
        QCOMPARE(admitted.ops.size(), 1);
        QCOMPARE(admitted.ops.first().value, u"\"fine\""_s);
    }

    // Of the same item at the same moment, an addition wins a removal, in
    // either order; a later removal wins, and a later addition brings it back.
    void anAdditionWinsARemovalAtTheSameMoment() {
        const SpaceOperation added = op("set:strokes", R"({"add":{"a":1}})", 10, "d1");
        const SpaceOperation removed = op("set:strokes", R"({"remove":["a"]})", 10, "d2");
        const SpaceOperation later = op("set:strokes", R"({"remove":["a"]})", 11, "d2");
        const SpaceOperation again = op("set:strokes", R"({"add":{"a":2}})", 12, "d1");
        for (const QList<SpaceOperation> &ops : {QList{added, removed}, QList{removed, added}})
            QCOMPARE(keys(PluginData::reduce(ops).first()), QStringList{"a"});
        QVERIFY(keys(PluginData::reduce(QList{added, later}).first()).isEmpty());
        QCOMPARE(PluginData::reduce(QList{added, later, again}).first().toObject().value("sets")["strokes"]["a"].toInt(), 2);
    }

    // A plugin reads its own things alone: another's of the same name are not its.
    void aPluginReadsOnlyItsOwn() {
        TwoComputers two;
        QVERIFY(PluginData::set(two.here, {.plugin = u"other"_s, .entity = u"page"_s, .id = u"p1"_s}, {{"title", "Theirs"}}).has_value());
        QCOMPARE(two.call("page", {"p1"}), QJsonValue(QJsonValue::Null));
        QVERIFY(two.call("title", {"p1", "Mine"}).toBool());
        QCOMPARE(title(two.call("page", {"p1"})), u"Mine"_s);
        QCOMPARE(title(PluginData::read(two.here, {.plugin = u"other"_s, .entity = u"page"_s, .id = u"p1"_s})), u"Theirs"_s);
    }
};

QTEST_MAIN(PluginDataTests)
#include "plugindata_tests.moc"
