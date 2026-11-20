// The reading statistics in the window (#68): the view of a year with a
// reading history, its goal set there and shown on the shelf, at every size,
// in German (whose words are a third longer), right to left, under fingers,
// and in a light theme and a dark one, as the layout tests look at the rest
// of the window (desk.h).
//
// OMABOOK_SHOTS=<folder> saves a picture of every state at every size.

#include <QDate>
#include <QQuickStyle>
#include <QtTest>
#include <chrono>

#include "desk.h"
#include "highlightcolors.h"
#include "readinggoal.h"
#include "readinglog.h"
#include "readingstatsview.h"

using namespace std::chrono_literals;

class StatsViewTests : public QObject {
    Q_OBJECT

    // Every problem found, by where it was found: "de 480x800 stats".
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
        desk.shot(QString("stats-%1%2-%3-%4x%5")
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

    // Three books read this year and one the year before, one of them
    // finished, with the time each day took.
    static void readSome(Desk &desk) {
        Library &library = *desk.backend->library();
        const QDate today = QDate::currentDate();
        const QStringList titles{"Die Hafenchronik der Laternenwärterin", "Moos", "Salzstraßen des Nordens"};
        for (qsizetype at = 0; at < titles.size(); ++at) {
            const QString file = desk.dir.filePath(QString("book%1.epub").arg(at));
            QVERIFY(Fixtures::chapteredEpub(file, titles.at(at), {{"Eins", "<h1>Eins</h1><p>Am Abend.</p>"}}));
            const qint64 id = library.add(file).id;
            QVERIFY(id > 0);
            for (int day = 0; day < 12; ++day)
                ReadingLog::add(library, id, day * 8.0, day * 8.0 + 8, std::chrono::minutes{20 + day * 3},
                                today.addDays(-(day * 9 + at)).toString(Qt::ISODate));
        }
        QVERIFY(library.setReading(1, "read", 4, today.addDays(-2).toString(Qt::ISODate)));
        desk.backend->reload();
    }

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void theStatisticsAtEverySizeInGermanRightToLeftUnderFingersLightAndDark() {
        struct Way {
            QByteArray language;
            QByteArray colors;
            bool touch;
        };
        for (const Way &way : {Way{"en", {}, false}, Way{"de", lightTheme, false}, Way{"ar", lightTheme, false}, Way{"de", {}, true}}) {
            Desk desk(way.language, way.colors, way.touch);
            readSome(desk);
            QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
            if (desk.window->minimumWidth() > 420 || desk.window->minimumHeight() > 360)
                desk.window->setMinimumSize(QSize(420, 360));
            // No goal yet: the shelf says nothing of one.
            QVERIFY(!desk.shows("shelfGoal"));
            for (const QSize &size : dialogSizes()) {
                QVERIFY(desk.resize(size));
                QVERIFY(desk.run("library.stats"));
                QTRY_VERIFY(desk.find("readingStats")->property("opened").toBool());
                QTRY_VERIFY(desk.shows("statsMonths"));
                look(desk, "year");
                QVERIFY(desk.closePopups());
            }
            // A goal set in the view, shown on the shelf.
            QVERIFY(desk.run("library.stats"));
            QTRY_VERIFY(desk.shows("statsGoalEdit"));
            QMetaObject::invokeMethod(desk.find("statsGoalEdit"), "clicked");
            QTRY_VERIFY(desk.shows("statsGoalSave"));
            QTest::qWait(300); // the dialog's opening, for the picture
            look(desk, "goal-editing");
            QMetaObject::invokeMethod(desk.find("statsGoalSave"), "clicked");
            // What the window said, if it couldn't (a library busy elsewhere).
            QTRY_VERIFY2(ReadingGoal::of(*desk.backend->library(), QDate::currentDate().year()).target == 24,
                         qPrintable(desk.backend->property("message").toString()));
            QTRY_VERIFY(desk.shows("statsGoalBar"));
            look(desk, "goal");
            QVERIFY(desk.closePopups());
            // Beside the number of books, where the shelf's header has room for that.
            QTRY_VERIFY(desk.shows("shelfGoal") || !desk.shows("bookCount"));
            for (const QSize &size : sizes()) {
                QVERIFY(desk.resize(size));
                look(desk, "shelf");
            }
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
        verdict();
    }

    // The bars stand out from the background as a mark should, in a light
    // theme and a dark one, whatever the accent.
    void theBarsStandOutInEveryTheme() {
        ReadingStatsView view;
        for (const char *background : {"#fafafa", "#ffffff", "#1e1e2e", "#000000", "#282828"})
            for (const char *accent : {"#005fcc", "#f5c2e7", "#fab387", "#000000", "#ffffff", "#7aa2f7", "#d8a657"}) {
                const QColor shown = view.mark(QColor(accent), QColor(background));
                QVERIFY2(HighlightColors::contrast(shown, QColor(background)) >= 3,
                         qPrintable(QString("%1 on %2 is %3").arg(accent, background, shown.name())));
            }
    }
};

QTEST_MAIN(StatsViewTests)
#include "statsview_tests.moc"
