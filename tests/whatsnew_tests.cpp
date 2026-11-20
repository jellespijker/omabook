// What's new (src/whatsnew.cpp) in the window: the note that offers what
// the newest release brought, and each of its cards, at every size, in
// German (whose words are a third longer), right to left and under
// fingers, as the layout tests look at the rest of the window (desk.h). A
// release's entries are looked at here before it goes out.
//
// OMABOOK_SHOTS=<folder> saves a picture of every state at every size.

#include <QJSValue>
#include <QQuickStyle>
#include <QVersionNumber>
#include <QtTest>

#include "desk.h"

namespace {

[[nodiscard]] QVersionNumber number(const QString &version) { return QVersionNumber::fromString(version); }

// The newest release this build knows of that brought anything, and the
// one before it that did: someone who last saw that one is offered the
// newest's.
struct Releases {
    QString newest, before;
    qsizetype cards = 0;
};

[[nodiscard]] Releases releases() {
    const QVersionNumber current = number(OMABOOK_VERSION);
    QVersionNumber newest, before;
    for (const Manual::Step &feature : Manual::features())
        if (number(feature.version) <= current)
            newest = std::max(newest, number(feature.version));
    for (const Manual::Step &feature : Manual::features())
        if (number(feature.version) < newest)
            before = std::max(before, number(feature.version));
    const auto cards =
        std::ranges::count_if(Manual::features(), [&](const Manual::Step &feature) { return number(feature.version) == newest; });
    return {newest.toString(), before.toString(), cards};
}

} // namespace

class WhatsNewTests : public QObject {
    Q_OBJECT

    // Every problem found, by where it was found: "de 480x800 news-1".
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
        desk.shot(QString("whatsnew-%1%2-%3-%4x%5")
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

    static bool touring(const Desk &desk) { return desk.find("tour")->property("running").toBool(); }

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void theNewestReleaseAtEverySizeInGermanRightToLeftAndUnderFingers() {
        const Releases release = releases();
        QVERIFY(release.cards > 0);
        struct Way {
            QByteArray language;
            bool touch;
        };
        for (const Way &way : {Way{"en", false}, Way{"de", false}, Way{"ar", false}, Way{"de", true}}) {
            Desk desk(way.language, way.language == "ar" ? lightTheme : QByteArray(), way.touch);
            QVERIFY(Fixtures::chapteredEpub(desk.dir.filePath("hafen.epub"), "Die Hafenchronik der Laternenwärterin",
                                            {{"Erstes Kapitel", "<h1>Erstes Kapitel</h1><p>Am Abend.</p>"}}));
            QVERIFY(desk.backend->library()->add(desk.dir.filePath("hafen.epub")).id > 0);
            desk.backend->reload();
            desk.settings->setNewsSeen(release.before);
            QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
            if (desk.window->minimumWidth() > 420 || desk.window->minimumHeight() > 360)
                desk.window->setMinimumSize(QSize(420, 360));
            // The note at the foot of the window that offers it.
            QTRY_VERIFY(desk.shows("tourNote"));
            for (const QSize &size : sizes()) {
                QVERIFY(desk.resize(size));
                look(desk, "offered");
            }
            // Each card of the newest release, at each size.
            for (const QSize &size : dialogSizes()) {
                QVERIFY(desk.resize(size));
                desk.call("startTour", "allnews");
                QTRY_VERIFY(touring(desk));
                const QVariant steps = desk.find("tour")->property("steps");
                QCOMPARE(steps.metaType() == QMetaType::fromType<QJSValue>() ? steps.value<QJSValue>().property("length").toInt()
                                                                             : steps.toList().size(),
                         release.cards);
                for (qsizetype card = 1; card <= release.cards; ++card) {
                    QTest::qWait(280); // the card moves to its step
                    look(desk, QString("news-%1").arg(card));
                    QMetaObject::invokeMethod(desk.find("tourNext"), "clicked");
                }
                QTRY_VERIFY(!touring(desk));
            }
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
        verdict();
    }
};

QTEST_MAIN(WhatsNewTests)
#include "whatsnew_tests.moc"
