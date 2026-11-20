// The contents beside the page stay in their pane (#122). Under fingers the
// row of the Contents and Highlights tabs and Close was wider than the pane,
// and the whole pane with it: the selected chapter's highlight, the list's
// scroll bar and Close ran over the page and the book's title beside it.
// Here a book of many chapters, one of them selected, is looked at in a
// narrow window like the one it was seen in and at every size the layout
// tests cover (desk.h), with a mouse and under fingers, in German and right
// to left.
//
// OMABOOK_SHOTS=<folder> saves a picture of every state at every size.

#include <QQuickStyle>
#include <QtTest>

#include "desk.h"

namespace {

[[nodiscard]] QRectF onScreen(const QQuickItem *item) { return item->mapRectToScene(QRectF(0, 0, item->width(), item->height())); }

[[nodiscard]] qint64 manyChapters(Desk &desk) {
    QList<Fixtures::Chapter> chapters{{"Prologue", "<h1>Prologue</h1><p>Mist.</p>"}};
    for (int i = 1; i <= 40; ++i)
        chapters.append({QString::number(i), QString("<h1>%1</h1><p>Weak lights peeked through the mist.</p>").arg(i)});
    const QString epub = desk.dir.filePath("chapters.epub");
    if (!Fixtures::chapteredEpub(epub, "The Final Empire", chapters))
        return 0;
    const qint64 book = desk.backend->library()->add(epub).id;
    desk.backend->reload();
    return book;
}

// Everything drawn in the pane, the selected row's highlight and the
// list's scroll bar among them, is across it.
[[nodiscard]] QStringList pastItsSides(QQuickItem *contents, const QRectF &pane) {
    QStringList problems;
    QList<QQuickItem *> items{contents};
    while (!items.isEmpty()) {
        QQuickItem *item = items.takeFirst();
        if (!item->isVisible() || item->width() < 1 || item->height() < 1)
            continue;
        items << item->childItems();
        // A bare item draws nothing: the list's own highlight, which
        // follows the current row with a lag, is one.
        if (qstrcmp(item->metaObject()->className(), "QQuickItem") == 0)
            continue;
        const QRectF at = onScreen(item);
        if (at.left() < pane.left() || at.right() > pane.right())
            problems << "past the pane's side: " + Survey::describe(item, at);
    }
    return problems;
}

// Its tabs and Close are in it, and clear of the header beside it (over
// the page, the pane covers the header too).
[[nodiscard]] QStringList outOfPlace(Desk &desk, const QRectF &pane, bool beside) {
    QStringList problems;
    const QQuickItem *title = desk.item("readerTitle");
    for (const char *name : {"paneTab_contents", "paneTab_annotations", "contentsClose"}) {
        const QQuickItem *item = desk.item(name);
        if (!item || !item->isVisible())
            continue;
        const QRectF at = onScreen(item);
        if (!pane.contains(at))
            problems << "outside the pane: " + Survey::describe(item, at);
        if (beside && title->isVisible() && at.intersects(onScreen(title)))
            problems << "over the header: " + Survey::describe(item, at);
    }
    return problems;
}

} // namespace

class ContentsPaneTests : public QObject {
    Q_OBJECT

    // The pane at one size, its list in the keyboard's hands, so that the
    // chapter being read is selected.
    static QStringList look(Desk &desk, const QSize &size) {
        if (!desk.resize(size))
            return {"the window could not be resized"};
        QMetaObject::invokeMethod(desk.find("contents"), "takeFocus");
        desk.settle();
        const QQuickItem *side = desk.item("contentsSide");
        if (!side || !side->isVisible())
            return {"the contents are not shown"};
        const QRectF pane = onScreen(side).adjusted(-0.5, -0.5, 0.5, 0.5);
        QStringList problems = pastItsSides(desk.item("contents"), pane) + outOfPlace(desk, pane, !side->property("over").toBool());
        Survey survey{desk.window, desk.touch, {}};
        survey.walk();
        problems << survey.problems;
        desk.shot(QString("%1%2-contents-%3x%4")
                      .arg(QString::fromLatin1(desk.language), desk.touch ? "-touch" : "")
                      .arg(size.width())
                      .arg(size.height()));
        return problems;
    }

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void theContentsStayInTheirPane_data() {
        QTest::addColumn<QByteArray>("language");
        QTest::addColumn<bool>("touch");
        QTest::newRow("en") << QByteArray("en") << false;
        QTest::newRow("en touch") << QByteArray("en") << true;
        QTest::newRow("de touch") << QByteArray("de") << true;
        QTest::newRow("ar touch") << QByteArray("ar") << true;
    }
    void theContentsStayInTheirPane() {
        QFETCH(QByteArray, language);
        QFETCH(bool, touch);
        Desk desk(language, {}, touch);
        const qint64 book = manyChapters(desk);
        QVERIFY(book > 0);
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        desk.reader->openBook(book);
        QTRY_VERIFY(desk.reader->isOpen());
        QTRY_VERIFY(desk.find("readerView"));
        desk.reader->goTo(5);
        desk.find("readerView")->setProperty("showIndex", true);
        QStringList found;
        // The window it was seen in first, then every size of the survey.
        for (const QSize &size : QList<QSize>{QSize(757, 1127)} + sizes())
            for (const QString &problem : look(desk, size))
                found << QString("%1x%2 — %3").arg(size.width()).arg(size.height()).arg(problem);
        found.removeDuplicates();
        QVERIFY2(found.isEmpty(), qPrintable(found.mid(0, 40).join('\n')));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }
};

QTEST_MAIN(ContentsPaneTests)
#include "contentspane_tests.moc"
