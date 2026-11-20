// A world from the command line in a library of several tenants (#112):
// `omabook graph show`, `find` and `add` with --scope read and add to it
// whole without --collection, `graph scopes` lists the worlds of every
// tenant, and the general graph still asks which tenant.

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include "graph.h"
#include "library.h"
#include "mcpharness.h"

namespace {

using McpHarness::Machine;

[[nodiscard]] QJsonObject json(const Machine::Ran &ran) { return QJsonDocument::fromJson(ran.output.toUtf8()).object(); }

// A story in the collection Shelf, whose world was met in it, and a person's book in none.
void storyOnAShelf(const Machine &machine) {
    Library library(machine.library);
    QVERIFY(library.open());
    BookMeta story;
    story.title = "The Glass Tithe";
    BookMeta person;
    person.title = "A Person's Book";
    const qint64 storyId = library.addEntry(story, true).id;
    QVERIFY(storyId > 0 && library.addEntry(person, true).id > 0);
    const auto shelf = library.addCollection("Shelf");
    QVERIFY(shelf && library.putInCollection(*shelf, storyId, true));
    QVERIFY(Graph(library).upsert("world:s1", "place", "Hollow", {}, {}, GraphPlace{storyId, 0, 0.1}) > 0);
}

} // namespace

class WorldCliTests : public QObject {
    Q_OBJECT

private slots:
    void aWorldIsReadWholeFromTheCommandLine() {
        const Machine machine;
        storyOnAShelf(machine);
        const QStringList in{"--library", machine.library, "--json"};
        const QJsonArray scopes = json(machine.omabook(QStringList{"graph", "scopes"} + in)).value("scopes").toArray();
        QCOMPARE(scopes.size(), 1);
        QCOMPARE(scopes.at(0).toObject().value("scope").toString(), QString("world:s1"));
        QCOMPARE(scopes.at(0).toObject().value("nodes").toInt(), 1);
        const QJsonArray nodes = json(machine.omabook(QStringList{"graph", "show", "--scope", "world:s1"} + in)).value("nodes").toArray();
        QCOMPARE(nodes.size(), 1);
        QCOMPARE(nodes.at(0).toObject().value("name").toString(), QString("Hollow"));
        QCOMPARE(machine.omabook(QStringList{"graph", "add", "--kind", "place", "--name", "Saltmere", "--scope", "world:s2"} + in).code, 0);
        QCOMPARE(json(machine.omabook(QStringList{"graph", "show", "--scope", "world:s2"} + in)).value("nodes").toArray().size(), 1);
        const Machine::Ran general = machine.omabook(QStringList{"graph", "show"} + in);
        QCOMPARE(general.code, 1);
        QVERIFY2(general.errors.contains("Shelf") && general.errors.contains("Unfiled"), qPrintable(general.errors));
    }
};

QTEST_MAIN(WorldCliTests)
#include "worldcli_tests.moc"
