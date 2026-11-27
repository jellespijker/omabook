// A number that is none, or out of range, is refused and changes nothing
// (#230): at the command line, from an agent over MCP, from a script and in the
// window. It never becomes 0, and a section past a book's end is not kept.

#include <QQuickStyle>
#include <QtTest>

#include "desk.h"
#include "devicedesk.h"
#include "library.h"
#include "mcpharness.h"
#include "numberoptions.h"

namespace {

// A machine with one book of five chapters, third in its series, read into its third.
struct Shelf {
    McpHarness::Machine machine;
    Library library{machine.library};
    qint64 book = 0;

    [[nodiscard]] bool stock() {
        const QString file = machine.dir.filePath("The Ferry.epub");
        if (!Fixtures::chapteredEpub(file, "The Ferry",
                                     {{"One", "<p>One.</p>"},
                                      {"Two", "<p>Two.</p>"},
                                      {"Three", "<p>Three.</p>"},
                                      {"Four", "<p>Four.</p>"},
                                      {"Five", "<p>Five.</p>"}})
            || !library.open())
            return false;
        book = library.add(file).id;
        if (book <= 0)
            return false;
        BookMeta meta = library.book(book)->meta;
        meta.series = "Tides";
        meta.seriesIndex = 3;
        library.setProgress(book, {.place = {.section = 2, .fraction = 0.5}, .percent = 50});
        return library.setMeta(book, meta) && library.setReading(book, "reading", 4, {});
    }
    [[nodiscard]] McpHarness::Machine::Ran run(QStringList words) const { return machine.omabook(words << "--library" << machine.library); }
    // The book as it was stocked; empty when it is, else what changed.
    [[nodiscard]] QString changed() const {
        const Book now = *library.book(book);
        const Progress read = library.progress(book);
        if (now.meta.seriesIndex != 3 || now.rating != 4)
            return QString("series %1, rating %2").arg(now.meta.seriesIndex).arg(now.rating);
        if (read.place.section != 2 || read.place.fraction != 0.5 || read.percent != 50)
            return QString("section %1, fraction %2, %3%").arg(read.place.section).arg(read.place.fraction).arg(read.percent);
        if (const auto marks = library.annotations(book).size(); marks != 0)
            return QString("%1 annotations").arg(marks);
        return {};
    }
};

} // namespace

class NumbersTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void onlyAWholeNumberInRangeIsOne() {
        QCOMPARE(Numbers::whole(u"3"), std::optional<qint64>(3));
        for (const char *text : {"", "three", "3.5", "-1", "3x", "99999999999999999999"})
            QVERIFY2(!Numbers::whole(QString::fromLatin1(text)), text);
        QCOMPARE(Numbers::decimal(u"2.5"), std::optional(2.5));
        for (const char *text : {"", "nan", "inf", "-1", "2,5", "half"})
            QVERIFY2(!Numbers::decimal(QString::fromLatin1(text)), text);
        QCOMPARE(Numbers::list(u"1, 3"), std::optional(QList<int>{1, 3}));
        QVERIFY(!Numbers::list(u"1,x"));
        QVERIFY(!Numbers::list(u"0"));
    }

    void theCommandLineRefusesIt_data() {
        QTest::addColumn<QStringList>("words");
        // What was refused, which the error names.
        QTest::addColumn<QString>("refused");
        const QList<std::pair<QStringList, QString>> commands{
            {{"set", "1", "--series-index", "three"}, "three"},
            {{"set", "1", "--series-index", "nan"}, "nan"},
            {{"set", "1", "--series-index", "-2", "--title", "Renamed"}, "-2"},
            {{"set", "1", "--rating", "five"}, "five"},
            {{"set", "1", "--rating", "9"}, "9"},
            {{"progress", "1", "--section", "abc"}, "abc"},
            {{"progress", "1", "--section", "-1"}, "-1"},
            {{"progress", "1", "--section", "5"}, "0 to 4"},
            {{"progress", "1", "--section", "999"}, "999"},
            {{"progress", "1", "--percent", "abc"}, "abc"},
            {{"progress", "1", "--percent", "101"}, "101"},
            {{"progress", "1", "--fraction", "half"}, "half"},
            {{"progress", "1", "--section", "1", "--updated", "yesterday"}, "yesterday"},
            {{"bookmarks", "1", "--add", "here", "--section", "abc"}, "abc"},
            {{"bookmarks", "1", "--add", "here", "--section", "999"}, "999"},
            {{"bookmarks", "1", "--add", "here", "--section", "1", "--fraction", "x"}, "\"x\""},
            {{"annotations", "1", "--add", "bookmark", "--section", "abc"}, "abc"},
            {{"annotations", "1", "--add", "bookmark", "--section", "999"}, "999"},
            {{"text", "1", "--section", "abc"}, "0 to 4"},
            {{"ask", "1", "Why?", "--upto", "abc"}, "abc"},
            {{"graph", "show", "one"}, "one"},
            {{"graph", "show", "1", "--upto", "abc"}, "abc"},
            {{"graph", "add", "--kind", "character", "--name", "Mira", "--book", "1", "--section", "abc"}, "abc"},
            {{"graph", "add", "--kind", "character", "--name", "Mira", "--book", "1", "--section", "999"}, "999"},
            {{"graph", "find", "Mira", "--limit", "many"}, "many"},
            {{"chronology", "1", "--upto", "1", "--fraction", "x"}, "\"x\""},
            {{"tidy", "--pick", "1,x", "--apply"}, "1,x"},
            {{"logs", "-n", "ten"}, "ten"},
            {{"serve", "--reach", "localhost", "--port", "abc"}, "abc"},
            {{"serve", "--reach", "localhost", "--port", "70000"}, "70000"},
            {{"plugin", "run", "nothing", "go", "--wait", "soon"}, "soon"},
        };
        for (const auto &[words, refused] : commands)
            QTest::newRow(qPrintable(words.join(' '))) << words << refused;
    }
    void theCommandLineRefusesIt() {
        QFETCH(const QStringList, words);
        QFETCH(const QString, refused);
        Shelf shelf;
        QVERIFY(shelf.stock());
        const auto ran = shelf.run(words);
        QVERIFY2(ran.code == 1, qPrintable(QString::number(ran.code) + ran.output + ran.errors));
        QVERIFY2(ran.errors.contains(refused), qPrintable(ran.output + ran.errors));
        QCOMPARE(shelf.changed(), QString());
        QCOMPARE(shelf.library.book(shelf.book)->meta.title, QString("The Ferry"));
    }

    void whatIsANumberIsStillTaken() {
        Shelf shelf;
        QVERIFY(shelf.stock());
        auto ran = shelf.run({"set", "1", "--series-index", "2.5", "--rating", "0"});
        QVERIFY2(ran.code == 0, qPrintable(ran.errors));
        QCOMPARE(shelf.library.book(shelf.book)->meta.seriesIndex, 2.5);
        ran = shelf.run({"set", "1", "--series-index", ""});
        QVERIFY2(ran.code == 0, qPrintable(ran.errors));
        QCOMPARE(shelf.library.book(shelf.book)->meta.seriesIndex, 0.0);
        ran = shelf.run({"progress", "1", "--section", "4", "--fraction", "1", "--percent", "100"});
        QVERIFY2(ran.code == 0, qPrintable(ran.errors));
        QCOMPARE(shelf.library.progress(shelf.book).place.section, 4);
        // A place another computer had, whose file of the book may be longer, comes as it was.
        ran = shelf.run({"progress", "1", "--section", "7", "--updated", "4102444800"});
        QVERIFY2(ran.code == 0, qPrintable(ran.errors));
        QCOMPARE(shelf.library.progress(shelf.book).place.section, 7);
    }

    void theDetailsFileRefusesIt() {
        Shelf shelf;
        QVERIFY(shelf.stock());
        const QString details = shelf.machine.dir.filePath("details.json");
        QVERIFY(Fixtures::write(details, R"({"series_index": "three"})"));
        const auto ran = shelf.run({"set", "1", "--meta", details});
        QVERIFY2(ran.code == 1 && ran.errors.contains("three"), qPrintable(ran.errors));
        QCOMPARE(shelf.changed(), QString());
    }

    void anAgentIsRefused() {
        Shelf shelf;
        QVERIFY(shelf.stock());
        McpHarness::Server server(shelf.machine);
        const qint64 id = shelf.book;
        const QList<std::pair<QString, QJsonObject>> calls{
            {"book_set_details", {{"id", id}, {"series_index", "three"}}},
            {"progress_set", {{"id", id}, {"section", 999}}},
            {"progress_set", {{"id", id}, {"section", 5}}},
            {"progress_set", {{"id", id}, {"section", 1e12}}},
            {"bookmark_add", {{"id", id}, {"section", 999}}},
            {"annotation_add", {{"id", id}, {"kind", "bookmark"}, {"section", 999}}},
            {"graph_add", {{"kind", "character"}, {"name", "Mira"}, {"book", id}, {"section", 999}}},
            {"book_add_entry", {{"title", "Elsewhere"}, {"paper", true}, {"rating", 9}}},
            {"reading_goal_set", {{"target", 1e12}}},
        };
        for (const auto &[tool, arguments] : calls)
            QVERIFY2(!server.failure(tool, arguments).isEmpty(), qPrintable(tool));
        QCOMPARE(shelf.changed(), QString());
        QCOMPARE(shelf.library.books().size(), 1);
        QVERIFY(server.failure("progress_set", {{"id", id}, {"section", 999}}).contains("0 to 4"));
    }

    void aScriptIsRefused_data() {
        QTest::addColumn<QByteArray>("call");
        QTest::newRow("series") << QByteArray("omabook.library.setMeta(1, {series_index: 'three'})");
        QTest::newRow("not a number") << QByteArray("omabook.library.setMeta(1, {series_index: NaN})");
        QTest::newRow("rating") << QByteArray("omabook.library.setReading(1, {rating: 'five'})");
        QTest::newRow("text") << QByteArray("omabook.library.text(1, 'abc')");
    }
    void aScriptIsRefused() {
        QFETCH(const QByteArray, call);
        Shelf shelf;
        QVERIFY(shelf.stock());
        const QString plugins = shelf.machine.dir.filePath("plugins");
        QVERIFY(QDir().mkpath(plugins + "/meddler"));
        QVERIFY(Fixtures::write(plugins + "/meddler/plugin.json", R"({"name": "Meddler", "version": "1.0", "script": "main.js"})"));
        QVERIFY(Fixtures::write(plugins + "/meddler/main.js", "function go() { " + call + "; }"));
        const auto ran = shelf.run({"plugin", "run", "meddler", "go", "--folder", plugins});
        QVERIFY2(ran.code == 1 && ran.errors.contains("should be a"), qPrintable(ran.output + ran.errors));
        QCOMPARE(shelf.changed(), QString());
    }

    void theWindowSaysSoAndSavesNothing() {
        Desk desk{"en", {}, false};
        BookMeta meta;
        meta.title = "The Salt Road";
        meta.series = "Salt";
        meta.seriesIndex = 3;
        const qint64 id = desk.backend->library()->addEntry(meta, true).id;
        QVERIFY(id > 0);
        desk.backend->reload();
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        desk.quicken();
        desk.call("editBook", id);
        QTRY_VERIFY(desk.find("edit")->property("opened").toBool());
        QObject *number = desk.find("editNumber");
        QVERIFY(number);
        number->setProperty("text", "abc");
        QMetaObject::invokeMethod(desk.find("edit"), "save");
        QVERIFY(desk.find("edit")->property("opened").toBool());
        QTRY_VERIFY(desk.shows("editNoNumber"));
        QVERIFY(desk.find("editNoNumber")->property("text").toString().contains("abc"));
        QCOMPARE(desk.backend->library()->book(id)->meta.seriesIndex, 3.0);
        // Neither does the window's side take one.
        desk.backend->saveDetails(id, {{"seriesIndex", "abc"}});
        QCOMPARE(desk.backend->library()->book(id)->meta.seriesIndex, 3.0);
        number->setProperty("text", "2,5");
        QMetaObject::invokeMethod(desk.find("edit"), "save");
        QTRY_VERIFY(!desk.find("edit")->property("opened").toBool());
        QCOMPARE(desk.backend->library()->book(id)->meta.seriesIndex, 2.5);
    }

    void aPortThatIsNoneKeepsTheOneThereWas() {
        Desk desk{"en", {}, false};
        desk.devices->set("mail_port", "2525");
        desk.devices->set("mail_port", "smtp");
        QCOMPARE(desk.devices->options().value("mail_port").toInt(), 2525);
        QVERIFY2(desk.devices->status().contains("smtp"), qPrintable(desk.devices->status()));
    }
};

QTEST_MAIN(NumbersTests)
#include "numbers_tests.moc"
