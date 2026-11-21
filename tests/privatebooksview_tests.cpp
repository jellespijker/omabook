// Books a plugin keeps private in the publish dialog (#127): which stay out
// and why, each with its way to let it go, the question asked before one
// goes, and the way back once it does. At every size, in a long language, a
// language that reads right to left, and under fingers, with nothing cut off
// or overlapping (the layout survey).

#include <QtTest>

#include "desk.h"
#include "privatebooks.h"
#include "spacedesk.h"

class PrivateBooksViewTests : public QObject {
    Q_OBJECT

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
        desk.shot(QString("private-%1%2-%3-%4x%5")
                      .arg(QString::fromLatin1(desk.language), desk.touch ? "-touch" : "", state)
                      .arg(desk.window->width())
                      .arg(desk.window->height()));
    }

private slots:
    void keptPrivateInThePublishDialogAtEverySize() {
        struct Way {
            QByteArray language;
            bool touch;
        };
        for (const Way &way : {Way{"de", false}, Way{"ar", false}, Way{"nl", true}}) {
            Desk desk(way.language, way.language == "ar" ? lightTheme : QByteArray(), way.touch);
            Library &library = *desk.backend->library();
            const QList<qint64> ids = stock(desk);
            QVERIFY(!ids.isEmpty());
            // One put in by hand, one its rule takes in, one let go: each says so.
            const auto shelf = library.addCollection("Notizen und Werkstoffe", "format:omanotebook");
            QVERIFY(shelf && library.putInCollection(*shelf, ids.first(), true));
            const qint64 byHand =
                Fixtures::privateNotebook(library, desk.dir.path(), "Notizen zur Wärmebehandlung von Stählen und Gusseisen");
            const qint64 byRule = Fixtures::privateNotebook(library, desk.dir.path(), "Gedanken");
            const qint64 letGo = Fixtures::privateNotebook(library, desk.dir.path(), "Rezepte für den Sommer am See");
            QVERIFY(byHand > 0 && byRule > 0 && letGo > 0);
            QVERIFY(library.putInCollection(*shelf, byHand, true) && PrivateBooks::share(library, letGo, true));
            QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
            if (desk.window->minimumWidth() > 420 || desk.window->minimumHeight() > 360)
                desk.window->setMinimumSize(QSize(420, 360));
            desk.quicken();
            desk.backend->reload();
            desk.backend->setCollection(*shelf);
            QVERIFY(desk.run("library.publish"));
            QTRY_COMPARE(desk.backend->spaces()->publishing().value("kept").toList().size(), 3);
            QTRY_VERIFY(desk.shows("publishKeptBook0"));
            for (const QSize &size : dialogSizes()) {
                QVERIFY(desk.resize(size));
                look(desk, "kept");
            }
            // Asked before one goes.
            QObject *dialog = desk.find("publishDialog");
            QVERIFY(dialog);
            dialog->setProperty("asking", dialog->property("kept").toList().first());
            QTRY_VERIFY(desk.shows("publishKeptAsk"));
            for (const QSize &size : dialogSizes()) {
                QVERIFY(desk.resize(size));
                look(desk, "asking");
            }
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
        const QStringList problems = found;
        found.clear();
        if (!problems.isEmpty())
            QFAIL(qPrintable(
                QString("%1 layout problems in %2 looks:\n%3").arg(problems.size()).arg(looked).arg(problems.mid(0, 40).join('\n'))));
    }
};

QTEST_MAIN(PrivateBooksViewTests)
#include "privatebooksview_tests.moc"
