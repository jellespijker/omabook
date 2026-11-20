// The graph window switches between the tenants of the graph (#96, ADR
// 0003): the library's top-level collections and Unfiled, the books in
// none, by key, pointer and finger, the one shown last remembered; the
// chronology opened from the graph shows and switches the same one; and
// with one tenant for the whole library there is nothing to switch. All of
// it at every size, in German, right to left and under fingers, as the
// layout tests look at the rest of the window (desk.h).
//
// OMABOOK_SHOTS=<folder> saves a picture of every state at every size.

#include <QPointingDevice>
#include <QQuickStyle>
#include <QtTest>
#include <string_view>

#include "desk.h"
#include "graphtenants.h"
#include "spaces.h"
#include "tenancy.h"

namespace {

// As long a name as a collection is given, to be hard on the header.
const QString harbourName = "The Harbour and Lantern Keepers' Library of Long Names";

// The made-up library split at its top: its fantasy in one collection, its
// nature books in another, and the rest in none (Unfiled).
struct Split {
    qint64 fantasy = 0, nature = 0, essays = 0;
    qint64 harbourId = 0, fieldId = 0;
    QString harbour, field;
};

[[nodiscard]] Split split(Desk &desk) {
    const QList<qint64> ids = stock(desk);
    Library &library = *desk.backend->library();
    const auto harbour = library.addCollection(harbourName, "tag:Fantasy");
    const auto field = library.addCollection("Fieldwork", "tag:Nature");
    if (!harbour || !field || ids.size() < 6)
        qFatal("the library could not be split: %s, %s, %d books", qPrintable(Fixtures::whyNot(harbour)),
               qPrintable(Fixtures::whyNot(field)), static_cast<int>(ids.size()));
    // A collection inside one is of its tenant, not a tenant of its own.
    std::ignore = library.addCollection("Moths", "tag:Nature", *field); // what it holds is checked by what the tenants say
    desk.backend->reload();
    return {.fantasy = ids.at(0),
            .nature = ids.at(1),
            .essays = ids.at(5),
            .harbourId = *harbour,
            .fieldId = *field,
            .harbour = Spaces::collectionId(library, *harbour),
            .field = Spaces::collectionId(library, *field)};
}

[[nodiscard]] QStringList idsOf(const GraphTenants &tenants) {
    QStringList ids;
    for (const QVariant &tenant : tenants.tenants())
        ids << tenant.toMap().value("id").toString();
    return ids;
}

[[nodiscard]] qint64 booksOf(const GraphTenants &tenants, const QString &id) {
    for (const QVariant &tenant : tenants.tenants())
        if (tenant.toMap().value("id").toString() == id)
            return tenant.toMap().value("books").toLongLong();
    return -1;
}

[[nodiscard]] QPoint middle(QQuickItem *item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint(); }

// One tenant for the whole library, as the settings say it (Tenancy reads
// the settings Omabook keeps, not the Desk's), until it goes out of scope.
class OneTenant {
public:
    OneTenant() { m_settings.setGraphOneTenant(true); }
    ~OneTenant() { m_settings.setGraphOneTenant(false); }
    OneTenant(const OneTenant &) = delete;
    OneTenant &operator=(const OneTenant &) = delete;
    OneTenant(OneTenant &&) = delete;
    OneTenant &operator=(OneTenant &&) = delete;

private:
    Settings m_settings;
};

} // namespace

class TenantSwitchTests : public QObject {
    Q_OBJECT

    // Every problem found, by where it was found: "de 480x800 graph".
    QStringList found;
    int looked = 0;

    void look(Desk &desk, const QString &state) {
        desk.settle(1);
        Survey survey{desk.window, desk.touch, {}};
        survey.walk();
        ++looked;
        const QString where = QString("%1%2 %3x%4 %5")
                                  .arg(QString::fromLatin1(desk.language), desk.touch ? " touch" : "")
                                  .arg(desk.window->width())
                                  .arg(desk.window->height())
                                  .arg(state);
        QSet<QString> once;
        for (const QString &problem : std::as_const(survey.problems))
            if (!once.contains(problem)) {
                once.insert(problem);
                found << where + " — " + problem;
            }
        desk.shot(QString("tenants-%1%2-%3-%4x%5")
                      .arg(QString::fromLatin1(desk.language), desk.touch ? "-touch" : "", state)
                      .arg(desk.window->width())
                      .arg(desk.window->height()));
    }
    void verdict() {
        const QStringList problems = found;
        found.clear();
        if (!problems.isEmpty())
            QFAIL(qPrintable(
                QString("%1 layout problems in %2 looks:\n%3").arg(problems.size()).arg(looked).arg(problems.mid(0, 40).join('\n'))));
    }

    static QObject *graph(Desk &desk) { return desk.find("libraryGraph")->property("browser").value<QObject *>(); }
    static QObject *chronology(Desk &desk) { return desk.find("chronology")->property("browser").value<QObject *>(); }
    static GraphTenants *tenants(Desk &desk) { return qobject_cast<GraphTenants *>(desk.find("graphTenants")); }
    // A press of a pointer, or a tap of a finger under fingers.
    static void press(Desk &desk, QQuickItem *item) {
        if (desk.touch) {
            static QPointingDevice *finger = QTest::createTouchDevice();
            QTest::touchEvent(desk.window, finger).press(0, middle(item), desk.window);
            QTest::touchEvent(desk.window, finger).release(0, middle(item), desk.window);
        } else {
            QTest::mouseClick(desk.window, Qt::LeftButton, Qt::NoModifier, middle(item));
        }
    }
    // The entry of the list of tenants that is up, by its place in it.
    static QQuickItem *entry(Desk &desk, const QString &list, int index) {
        QQuickItem *item = nullptr;
        QMetaObject::invokeMethod(desk.find(list), "itemAtIndex", Q_RETURN_ARG(QQuickItem *, item), Q_ARG(int, index));
        return item;
    }

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void theTenantsAreTheTopLevelCollectionsThenUnfiled() {
        Desk desk("en", {}, false);
        const Split split = ::split(desk);
        GraphTenants tenants;
        tenants.setBackend(&*desk.backend);
        const QString unfiled = Tenant::unfiled().globalId();
        QCOMPARE(idsOf(tenants), (QStringList{split.harbour, split.field, unfiled}));
        QVERIFY(tenants.switchable());
        QCOMPARE(tenants.tenants().first().toMap().value("name").toString(), harbourName);
        QVERIFY(tenants.tenants().last().toMap().value("unfiled").toBool());
        // Each counts its books, the collections inside it with it, and
        // the rest are Unfiled's.
        const Library &library = *desk.backend->library();
        const auto held = [&library](qint64 id) { return static_cast<qint64>(library.collection(id)->of(library.books()).size()); };
        QCOMPARE(booksOf(tenants, split.harbour), held(split.harbourId));
        QCOMPARE(booksOf(tenants, split.field), held(split.fieldId));
        QCOMPARE(booksOf(tenants, unfiled), library.books().size() - held(split.harbourId) - held(split.fieldId));
        // The first is shown until another is chosen.
        QCOMPARE(tenants.current(), split.harbour);
        QCOMPARE(tenants.shown().value("name").toString(), harbourName);
    }

    void aLibraryWithoutCollectionsIsOneTenantWithNothingToSwitch() {
        Desk desk("en", {}, false);
        std::ignore = stock(desk); // what it holds is counted below
        desk.backend->reload();
        GraphTenants tenants;
        tenants.setBackend(&*desk.backend);
        QCOMPARE(idsOf(tenants), QStringList{Tenant::unfiled().globalId()});
        QVERIFY(!tenants.switchable());
        QCOMPARE(booksOf(tenants, Tenant::unfiled().globalId()), desk.backend->library()->books().size());
    }

    void theOneShownIsRememberedAndStepsRound() {
        Desk desk("en", {}, false);
        const Split split = ::split(desk);
        const QString unfiled = Tenant::unfiled().globalId();
        {
            GraphTenants tenants;
            tenants.setBackend(&*desk.backend);
            QSignalSpy changed(&tenants, &GraphTenants::changed);
            tenants.choose(split.field);
            QCOMPARE(tenants.current(), split.field);
            QCOMPARE(changed.size(), 1);
            QCOMPARE(desk.settings->graphTenant(desk.backend->library()->root()), split.field);
            // What is no tenant is no choice.
            tenants.choose("nonsense");
            QCOMPARE(tenants.current(), split.field);
            QCOMPARE(changed.size(), 1);
        }
        // The next time, the one shown last.
        GraphTenants again;
        again.setBackend(&*desk.backend);
        QCOMPARE(again.current(), split.field);
        again.step(1);
        QCOMPARE(again.current(), unfiled);
        again.step(1);
        QCOMPARE(again.current(), split.harbour);
        again.step(-1);
        QCOMPARE(again.current(), unfiled);
    }

    void aBookIsSeenInATenantOfItsOwn() {
        Desk desk("en", {}, false);
        const Split split = ::split(desk);
        GraphTenants tenants;
        tenants.setBackend(&*desk.backend);
        QCOMPARE(tenants.current(), split.harbour);
        QCOMPARE(tenants.tenantOf(split.fantasy), split.harbour);
        QCOMPARE(tenants.tenantOf(split.nature), split.field);
        QCOMPARE(tenants.tenantOf(split.essays), Tenant::unfiled().globalId());
        // A book in two is seen in the one shown, when that is one of them.
        QVERIFY(desk.backend->library()->putInCollection(split.fieldId, split.fantasy, true));
        tenants.choose(split.field);
        QCOMPARE(tenants.tenantOf(split.fantasy), split.field);
        tenants.choose(split.harbour);
        QCOMPARE(tenants.tenantOf(split.fantasy), split.harbour);
    }

    void aCollectionThatGoesGivesWayToTheFirst() {
        Desk desk("en", {}, false);
        const Split split = ::split(desk);
        GraphTenants tenants;
        tenants.setBackend(&*desk.backend);
        tenants.choose(split.field);
        // The collection inside it moves up, a tenant of its own then.
        QVERIFY(desk.backend->library()->removeCollection(split.fieldId));
        desk.backend->reload();
        QTRY_VERIFY(!idsOf(tenants).contains(split.field));
        QCOMPARE(idsOf(tenants).size(), 3);
        QCOMPARE(tenants.current(), split.harbour);
    }

    void oneTenantForTheWholeLibraryHasNothingToSwitch() {
        Desk desk("en", {}, false);
        const Split split = ::split(desk);
        GraphTenants tenants;
        tenants.setBackend(&*desk.backend);
        tenants.choose(split.field);
        {
            const OneTenant one;
            tenants.refresh();
            QCOMPARE(idsOf(tenants), QStringList{Tenant::wholeLibrary().globalId()});
            QVERIFY(!tenants.switchable());
            QCOMPARE(tenants.current(), Tenant::wholeLibrary().globalId());
            // Every book is in it, as Tenancy follows the choice at once.
            QCOMPARE(booksOf(tenants, Tenant::wholeLibrary().globalId()), desk.backend->library()->books().size());
        }
        // Back to tenants, the one chosen last is shown again.
        tenants.refresh();
        QCOMPARE(idsOf(tenants).size(), 3);
        QCOMPARE(tenants.current(), split.field);
    }

    void theGraphAndItsChronologySwitchByKeyPointerAndFinger() {
        for (const bool touch : {false, true}) {
            Desk desk("en", {}, touch);
            const Split split = ::split(desk);
            const QString unfiled = Tenant::unfiled().globalId();
            QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
            QVERIFY(desk.resize(QSize(1180, 780)));
            desk.quicken();
            GraphTenants *chosen = tenants(desk);
            QVERIFY(chosen);
            QTRY_COMPARE(chosen->current(), split.harbour);

            // The library's graph is the first tenant's, named at the top.
            desk.call("showGraph", 0);
            QTRY_VERIFY(desk.shows("libraryGraph"));
            QObject *browser = graph(desk);
            QCOMPARE(browser->property("tenant").toString(), split.harbour);
            QTRY_VERIFY(desk.shows("graphTenant"));
            QCOMPARE(desk.find("graphTenantName")->property("text").toString(), harbourName);

            // The keys: to the next, and back.
            QVERIFY(desk.run("graph.tenant_next"));
            QCOMPARE(chosen->current(), split.field);
            QTRY_COMPARE(browser->property("tenant").toString(), split.field);
            QCOMPARE(browser->property("showing").toString(), QString("scope"));
            QCOMPARE(desk.find("graphTenantName")->property("text").toString(), QString("Fieldwork"));
            QVERIFY(desk.run("graph.tenant_previous"));
            QTRY_COMPARE(browser->property("tenant").toString(), split.harbour);

            // The list, by its key: typing narrows it, Enter takes it.
            QVERIFY(desk.run("graph.tenant"));
            QVERIFY(desk.waitForPopup());
            QVERIFY(desk.find("graphTenantChoice")->property("opened").toBool());
            QTRY_COMPARE(desk.find("graphTenantList")->property("count").toInt(), 3);
            QTRY_VERIFY(desk.find("graphTenantField")->property("activeFocus").toBool());
            for (const char letter : std::string_view("unfil"))
                QTest::keyClick(desk.window, letter);
            QTRY_COMPARE(desk.find("graphTenantList")->property("count").toInt(), 1);
            QTest::keyClick(desk.window, Qt::Key_Return);
            QTRY_COMPARE(browser->property("tenant").toString(), unfiled);
            QVERIFY(desk.closePopups());
            QCOMPARE(desk.find("graphTenantName")->property("text").toString(), QString("Unfiled"));

            // The list, by a click or a tap on the name, and a click or a tap on one in it.
            press(desk, desk.item("graphTenant"));
            QVERIFY(desk.waitForPopup());
            QQuickItem *second = entry(desk, "graphTenantList", 1);
            QVERIFY(second);
            press(desk, second);
            QTRY_COMPARE(browser->property("tenant").toString(), split.field);
            QTRY_VERIFY(desk.popups().isEmpty());

            // The chronology opened from the graph is of the same tenant,
            // and a switch there is the graph's too.
            QVERIFY(desk.run("graph.chronology"));
            QTRY_VERIFY(desk.shows("chronologyCanvas"));
            QCOMPARE(chronology(desk)->property("tenant").toString(), split.field);
            QTRY_VERIFY(desk.shows("chronologyTenant"));
            QVERIFY(desk.run("chronology.tenant_next"));
            QTRY_COMPARE(chronology(desk)->property("tenant").toString(), unfiled);
            QCOMPARE(chronology(desk)->property("showing").toString(), QString("scope"));
            press(desk, desk.item("chronologyTenant"));
            QVERIFY(desk.waitForPopup());
            QVERIFY(desk.find("chronologyTenantChoice")->property("opened").toBool());
            press(desk, entry(desk, "chronologyTenantList", 0));
            QTRY_COMPARE(chronology(desk)->property("tenant").toString(), split.harbour);
            QTRY_VERIFY(desk.popups().isEmpty());
            QVERIFY(desk.run("chronology.close"));
            QTRY_VERIFY(desk.shows("libraryGraph"));
            QCOMPARE(browser->property("tenant").toString(), split.harbour);

            // A book's graph opens in a tenant of its own, which is then the one shown.
            desk.call("showGraph", split.nature);
            QTRY_COMPARE(browser->property("showing").toString(), QString("book"));
            QCOMPARE(browser->property("tenant").toString(), split.field);
            QCOMPARE(chosen->current(), split.field);
            // Its chronology too, from the shelf.
            QMetaObject::invokeMethod(desk.find("libraryGraph"), "done");
            QMetaObject::invokeMethod(desk.item("chronologyLayer"), "show", Q_ARG(QVariant, "book"), Q_ARG(QVariant, split.essays));
            QTRY_VERIFY(desk.shows("chronologyCanvas"));
            QCOMPARE(chronology(desk)->property("tenant").toString(), unfiled);
            QCOMPARE(chosen->current(), unfiled);
            QVERIFY(desk.run("chronology.close"));

            // Remembered for the next time.
            QCOMPARE(desk.settings->graphTenant(desk.backend->library()->root()), unfiled);
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
    }

    void withOneTenantThereIsNoSwitch() {
        Desk desk("en", {}, false);
        std::ignore = ::split(desk); // its collections are what one tenant ignores
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        const OneTenant one;
        QMetaObject::invokeMethod(tenants(desk), "refresh");
        desk.call("showGraph", 0);
        QTRY_VERIFY(desk.shows("libraryGraph"));
        QTRY_COMPARE(graph(desk)->property("tenant").toString(), Tenant::wholeLibrary().globalId());
        desk.settle();
        QVERIFY(!desk.shows("graphTenant"));
        QVERIFY(desk.shows("graphTitle"));
        QVERIFY(!desk.run("graph.tenant"));
        QVERIFY(!desk.run("graph.tenant_next"));
        QVERIFY(desk.run("graph.chronology"));
        QTRY_VERIFY(desk.shows("chronologyCanvas"));
        QVERIFY(!desk.shows("chronologyTenant"));
        QVERIFY(!desk.run("chronology.tenant"));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void theSwitchAtEverySizeInGermanRightToLeftAndUnderFingers() {
        struct Way {
            QByteArray language;
            bool touch;
        };
        for (const Way &way : {Way{"en", false}, Way{"de", false}, Way{"ar", false}, Way{"de", true}}) {
            Desk desk(way.language, way.language == "ar" ? lightTheme : QByteArray(), way.touch);
            const Split split = ::split(desk);
            QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
            if (desk.window->minimumWidth() > 420 || desk.window->minimumHeight() > 360)
                desk.window->setMinimumSize(QSize(420, 360));
            desk.quicken();
            desk.call("showGraph", 0);
            QTRY_VERIFY(desk.shows("libraryGraph"));
            for (const QSize &size : sizes()) {
                QVERIFY(desk.resize(size));
                // The long name, and Unfiled.
                tenants(desk)->choose(split.harbour);
                look(desk, "graph");
                tenants(desk)->choose(Tenant::unfiled().globalId());
                look(desk, "graph-unfiled");
                // A book's graph, its title beside the name.
                desk.call("showGraph", split.fantasy);
                QTRY_COMPARE(graph(desk)->property("showing").toString(), QString("book"));
                look(desk, "graph-book");
                desk.call("showGraph", 0);
            }
            for (const QSize &size : dialogSizes()) {
                QVERIFY(desk.resize(size));
                QVERIFY(desk.run("graph.tenant"));
                QVERIFY(desk.waitForPopup());
                look(desk, "graph-tenants");
                QVERIFY(desk.closePopups());
            }
            tenants(desk)->choose(split.harbour);
            QVERIFY(desk.run("graph.chronology"));
            QTRY_VERIFY(desk.shows("chronologyCanvas"));
            for (const QSize &size : sizes()) {
                QVERIFY(desk.resize(size));
                look(desk, "chronology");
            }
            for (const QSize &size : dialogSizes()) {
                QVERIFY(desk.resize(size));
                QVERIFY(desk.run("chronology.tenant"));
                QVERIFY(desk.waitForPopup());
                look(desk, "chronology-tenants");
                QVERIFY(desk.closePopups());
            }
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
        verdict();
    }
};

QTEST_MAIN(TenantSwitchTests)
#include "tenantswitch_tests.moc"
