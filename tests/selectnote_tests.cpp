// The key of a margin note writes one on words already selected with the
// keyboard, however they came to be selected (#271): after V, which selects
// for the menu, as after the note's own key.

#include <QQuickStyle>
#include <QtTest>

#include "desk.h"
#include "library.h"

class SelectNoteTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void theNoteKeyWritesOnWhatIsSelected_data() {
        QTest::addColumn<int>("start");
        QTest::addColumn<Qt::KeyboardModifiers>("modifiers");
        QTest::newRow("selected with V") << int(Qt::Key_V) << Qt::KeyboardModifiers(Qt::NoModifier);
        QTest::newRow("selected for a note") << int(Qt::Key_N) << Qt::KeyboardModifiers(Qt::ShiftModifier);
    }

    void theNoteKeyWritesOnWhatIsSelected() {
        QFETCH(const int, start);
        QFETCH(const Qt::KeyboardModifiers, modifiers);
        Desk desk{"en", {}, false};
        const QList<qint64> ids = stock(desk);
        QVERIFY(!ids.isEmpty());
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        desk.quicken();
        QVERIFY(desk.resize(QSize(1500, 900)));
        desk.reader->openBook(ids.first());
        QTRY_VERIFY(desk.reader->isOpen() && desk.shows("flowText"));
        desk.settle();
        QTest::keyClick(desk.window, static_cast<Qt::Key>(start), modifiers);
        QTRY_VERIFY(desk.find("readerView")->property("selecting").toBool());
        const auto selected = [&desk] { return desk.find("flowText")->property("selectedText").toString(); };
        const QString first = selected();
        QTest::keyClick(desk.window, Qt::Key_Right, Qt::ShiftModifier);
        QTRY_VERIFY(selected().size() > first.size());
        const QString words = selected();
        QTest::keyClick(desk.window, Qt::Key_N, Qt::ShiftModifier);
        const auto typing = [&desk] {
            const QQuickItem *focused = desk.window->activeFocusItem();
            return focused && focused->objectName() == "noteField";
        };
        QTRY_VERIFY(typing());
        QVERIFY(!desk.find("readerView")->property("selecting").toBool());
        for (const char letter : QByteArray("Mine"))
            QTest::keyClick(desk.window, letter);
        QTest::keyClick(desk.window, Qt::Key_Escape);
        Library *library = desk.backend->library();
        QTRY_COMPARE(library->annotations(ids.first(), "margin-note").size(), 1);
        const Annotation note = library->annotations(ids.first(), "margin-note").first();
        QCOMPARE(note.note, QString("Mine"));
        QCOMPARE(note.quote.simplified(), words.simplified());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }
};

QTEST_MAIN(SelectNoteTests)
#include "selectnote_tests.moc"
