// A plugin's thing put back as an earlier copy of its library had it (#209):
// a notebook of the notebooks plugin with its pages and what is on them, or
// one page, as the plugin keeps them in the change log (ADR 0002). What was
// removed since comes back, what changed goes back, and what was made since
// stays. Two libraries of the test's own, the second a copy of the first.

#include <QDir>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

#include "fixtures.h"
#include "library.h"
#include "plugindata.h"
#include "pluginrestore.h"
#include "spaces.h"

using Fixtures::whyNot;

namespace {

[[nodiscard]] PluginData::Thing notebooks(const QString &entity, const QString &id) {
    return {.plugin = "notebooks", .entity = entity, .id = id};
}

[[nodiscard]] bool put(const Library &library, const QString &entity, const QString &id, const QJsonObject &fields) {
    return PluginData::set(library, notebooks(entity, id), fields).has_value();
}

[[nodiscard]] bool strokes(const Library &library, const QString &page, const QJsonObject &added, const QStringList &removed = {}) {
    return PluginData::change(library, notebooks("page", page), "strokes", {.added = added, .removed = removed}).has_value();
}

[[nodiscard]] QJsonObject read(const Library &library, const QString &entity, const QString &id) {
    return PluginData::read(library, notebooks(entity, id)).toObject();
}

// A library, and a copy of it as it stands: the snapshot's side, and the side that changes on.
struct Copied {
    QTemporaryDir dir;
    std::unique_ptr<Library> before = std::make_unique<Library>(dir.filePath("Before"));
    std::unique_ptr<Library> after;

    [[nodiscard]] bool copy() {
        if (!QDir().mkpath(dir.filePath("After")) || !before->copyInto(Library::databaseIn(dir.filePath("After"))))
            return false;
        after = std::make_unique<Library>(dir.filePath("After"));
        return after->open();
    }
};

// A notebook "Physics" of two pages, the first with two strokes and a text
// block, the second with its recognised text; and a notebook of its own,
// "Poems", with a page.
[[nodiscard]] bool twoNotebooks(const Library &library) {
    return put(library, "notebook", "n1", {{"title", "Physics"}, {"created", "2026-11-01"}})
           && put(library, "page", "p1", {{"notebook", "n1"}, {"order", 1}}) && strokes(library, "p1", {{"s1", "AAA"}, {"s2", "BBB"}})
           && put(library, "block", "t1", {{"page", "p1"}, {"text", "Momentum is kept."}})
           && put(library, "page", "p2", {{"notebook", "n1"}, {"order", 2}}) && put(library, "recognised", "p2", {{"text", "F = m a"}})
           && put(library, "notebook", "n2", {{"title", "Poems"}}) && put(library, "page", "p3", {{"notebook", "n2"}, {"order", 1}});
}

} // namespace

class PluginRestoreTests : public QObject {
    Q_OBJECT

private slots:
    // The notebook as the copy has it: its title back, its removed page back
    // with what was on it, its erased stroke back, its typed words back. The
    // page and the stroke made since stay, and the other notebook is not touched.
    void aNotebookIsPutBackWithItsPagesLosingNothingMadeSince() {
        Copied library;
        QVERIFY(library.before->open());
        QVERIFY(twoNotebooks(*library.before));
        QVERIFY(library.copy());
        Library &live = *library.before;
        QVERIFY(put(live, "notebook", "n1", {{"title", "Chemistry"}}));
        QVERIFY(PluginData::remove(live, notebooks("page", "p2")).has_value());
        QVERIFY(strokes(live, "p1", {{"s3", "CCC"}}, {"s1"}));
        QVERIFY(put(live, "block", "t1", {{"text", "Momentum is lost."}}));
        QVERIFY(put(live, "page", "p4", {{"notebook", "n1"}, {"order", 3}}));
        QVERIFY(put(live, "notebook", "n2", {{"title", "Songs"}}));

        const auto plan = PluginRestore::plan(live, *library.after, notebooks("notebook", "n1"));
        QVERIFY2(plan, qPrintable(whyNot(plan)));
        QVERIFY(!plan->empty());
        QCOMPARE(plan->comingBack.value("page"), 1);
        QCOMPARE(plan->changingBack.value("notebook"), 1);
        const auto restored = PluginRestore::apply(live, *plan);
        QVERIFY2(restored, qPrintable(whyNot(restored)));
        QVERIFY(restored->things >= 4);

        QCOMPARE(read(live, "notebook", "n1")["fields"].toObject()["title"].toString(), QString("Physics"));
        QCOMPARE(read(live, "page", "p2")["fields"].toObject()["notebook"].toString(), QString("n1"));
        QCOMPARE(read(live, "recognised", "p2")["fields"].toObject()["text"].toString(), QString("F = m a"));
        const QJsonObject drawn = read(live, "page", "p1")["sets"].toObject()["strokes"].toObject();
        QCOMPARE(drawn.keys(), (QStringList{"s1", "s2", "s3"}));
        QCOMPARE(read(live, "block", "t1")["fields"].toObject()["text"].toString(), QString("Momentum is kept."));
        QCOMPARE(read(live, "page", "p4")["fields"].toObject()["notebook"].toString(), QString("n1"));
        QCOMPARE(read(live, "notebook", "n2")["fields"].toObject()["title"].toString(), QString("Songs"));

        // Put back, there is nothing more to put back.
        const auto again = PluginRestore::plan(live, *library.after, notebooks("notebook", "n1"));
        QVERIFY(again);
        QVERIFY(again->empty());
    }

    // One page, and what is on it, back; not its notebook's title.
    void aPageIsPutBackAlone() {
        Copied library;
        QVERIFY(library.before->open());
        QVERIFY(twoNotebooks(*library.before));
        QVERIFY(library.copy());
        Library &live = *library.before;
        QVERIFY(put(live, "notebook", "n1", {{"title", "Chemistry"}}));
        QVERIFY(PluginData::remove(live, notebooks("page", "p1")).has_value());
        QVERIFY(PluginData::remove(live, notebooks("block", "t1")).has_value());

        const auto plan = PluginRestore::plan(live, *library.after, notebooks("page", "p1"));
        QVERIFY2(plan, qPrintable(whyNot(plan)));
        QCOMPARE(plan->comingBack.value("page"), 1);
        QCOMPARE(plan->comingBack.value("block"), 1);
        QVERIFY(!plan->changingBack.contains("notebook") && !plan->comingBack.contains("notebook"));
        QVERIFY(PluginRestore::apply(live, *plan));
        QCOMPARE(read(live, "page", "p1")["sets"].toObject()["strokes"].toObject().keys(), (QStringList{"s1", "s2"}));
        QCOMPARE(read(live, "block", "t1")["fields"].toObject()["text"].toString(), QString("Momentum is kept."));
        QCOMPARE(read(live, "notebook", "n1")["fields"].toObject()["title"].toString(), QString("Chemistry"));
    }

    // What the copy doesn't hold is refused, and nothing is written.
    void aThingTheCopyLacksIsRefused() {
        Copied library;
        QVERIFY(library.before->open());
        QVERIFY(twoNotebooks(*library.before));
        QVERIFY(library.copy());
        const auto plan = PluginRestore::plan(*library.before, *library.after, notebooks("notebook", "n9"));
        QVERIFY(!plan);
        QCOMPARE(plan.error().code(), Failure::Code::RestoreThingNotFound);
    }
};

QTEST_GUILESS_MAIN(PluginRestoreTests)
#include "pluginrestore_tests.moc"
