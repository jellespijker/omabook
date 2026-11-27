// The reader's find lists the first 200 places and says when the text has
// more, with a way to see them all (#270): never "200 places" as if that were
// all of them.

#include <QQuickStyle>
#include <QtTest>

#include "desk.h"
#include "fixtures.h"
#include "library.h"

class FindAllTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void moreThanTheFirstPlacesAreSaidAndShown_data() {
        QTest::addColumn<int>("places");
        QTest::newRow("exactly the first") << 200;
        QTest::newRow("one more") << 201;
        QTest::newRow("many more") << 400;
    }

    void moreThanTheFirstPlacesAreSaidAndShown() {
        QFETCH(const int, places);
        Desk desk("en", {}, false);
        // Once in every paragraph.
        const QString file = desk.dir.filePath("Chronicle.txt");
        QByteArray text = "CHAPTER 1\n\n";
        for (int i = 0; i < places; ++i)
            text += "Paragraph " + QByteArray::number(i) + " tells of quicksilver.\n\n";
        QVERIFY(Fixtures::write(file, text));
        const qint64 id = desk.backend->library()->add(file).id;
        QVERIFY(id > 0);
        desk.backend->reload();
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        desk.quicken();
        desk.reader->openBook(id);
        QTRY_VERIFY(desk.reader->isOpen() && desk.shows("flowText"));
        QCOMPARE(desk.reader->find("quicksilver").size(), 200);
        QCOMPARE(desk.reader->find("quicksilver", 0).size(), places);

        QVERIFY(desk.run("reader.find"));
        QVERIFY(desk.waitForPopup());
        desk.find("findWords")->setProperty("text", "quicksilver");
        QMetaObject::invokeMethod(desk.find("findWords"), "accepted");
        const auto said = [&desk] { return desk.find("findCount")->property("text").toString(); };
        const auto listed = [&desk] { return desk.find("findList")->property("count").toInt(); };
        QTRY_COMPARE(listed(), 200);
        if (places == 200) {
            QCOMPARE(said(), QString("200 places"));
            QVERIFY(!desk.shows("findAll"));
        } else {
            QCOMPARE(said(), QString("200 places shown, and there are more"));
            QTRY_VERIFY(desk.shows("findAll"));
            desk.shot("find-first-places");
            QMetaObject::invokeMethod(desk.find("findAll"), "clicked");
            QTRY_COMPARE(listed(), places);
            QCOMPARE(said(), QString("%1 places").arg(places));
            QVERIFY(!desk.shows("findAll"));
            // The keyboard is on the first of those that were not listed.
            QCOMPARE(desk.find("findList")->property("currentIndex").toInt(), 200);
            QVERIFY(desk.find("findList")->property("activeFocus").toBool());
        }
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }
};

QTEST_MAIN(FindAllTests)
#include "findall_tests.moc"
