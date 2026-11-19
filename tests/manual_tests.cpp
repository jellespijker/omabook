#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QVersionNumber>
#include <QtTest>

#include "customization.h"
#include "fixtures.h"
#include "manual.h"

// What Omabook says about itself (manual.h): the tour, what is new and the
// help's pages name commands and parts of the window. A rename must fail
// here, not leave a card pointing at nothing.
class ManualTests : public QObject {
    Q_OBJECT

    // Every objectName a view of the window has.
    [[nodiscard]] static QSet<QString> objectNames() {
        const QString sources = QFileInfo(QFINDTESTDATA("../src/Main.qml")).absolutePath();
        QSet<QString> names{"book"};
        static const QRegularExpression objectName("objectName:\\s*\"([A-Za-z0-9_-]+)\"");
        for (const QFileInfo &info : QDir(sources).entryInfoList({"*.qml"}, QDir::Files)) {
            QFile file(info.filePath());
            if (!file.open(QIODevice::ReadOnly))
                qFatal("can't read %s", qPrintable(info.filePath()));
            for (auto it = objectName.globalMatch(QString::fromUtf8(file.readAll())); it.hasNext();)
                names << it.next().captured(1);
        }
        return names;
    }

    [[nodiscard]] static QSet<QString> commandIds() {
        QSet<QString> commands;
        for (const Customization::Command &command : Customization::commands())
            commands << command.id;
        return commands;
    }

    // What a step points at, in the window and among the commands.
    [[nodiscard]] static QStringList pointsAtWhatIsThere(const QString &what, const Manual::Step &step, const QSet<QString> &names,
                                                         const QSet<QString> &commands) {
        QStringList problems;
        for (const QString &spot : step.spot.split('|', Qt::SkipEmptyParts))
            if (!names.contains(spot))
                problems << what + "points at \"" + spot + "\", which no view has as an objectName";
        if (!step.command.isEmpty() && !commands.contains(step.command))
            problems << what + "there is no command " + step.command;
        for (const QString &command : Manual::mentioned(step.text))
            if (!commands.contains(command))
                problems << what + "its text names {" + command + "}, which is no command";
        if (step.text.contains('{') && Manual::mentioned(step.text).size() != step.text.count('{'))
            problems << what + "a brace in its text isn't a command";
        return problems;
    }

    // A step's own fields. A feature may wait for the release after this
    // build, as one announced before its version is numbered does.
    [[nodiscard]] static QStringList wellFormed(const QString &what, const Manual::Step &step, bool feature) {
        QStringList problems;
        if (step.title.isEmpty() || step.text.isEmpty())
            problems << what + "needs a title and a text";
        if (!QStringList{"", "library", "reader"}.contains(step.place))
            problems << what + "its place is \"library\", \"reader\" or nothing";
        if (!QStringList{"", "empty", "books", "touch"}.contains(step.when))
            problems << what + "when is \"empty\", \"books\", \"touch\" or nothing";
        const QVersionNumber built = QVersionNumber::fromString(OMABOOK_VERSION);
        const QVersionNumber next(built.majorVersion(), built.minorVersion(), built.microVersion() + 1);
        const QVersionNumber version = QVersionNumber::fromString(step.version);
        if (feature && (version.segmentCount() != 3 || version > next))
            problems << what + "arrives in \"" + step.version + "\", which is after " + next.toString() + ", the release after this one";
        return problems;
    }

    [[nodiscard]] static QStringList stepProblems(const QString &where, const QList<Manual::Step> &steps, bool feature) {
        const QSet<QString> names = objectNames();
        const QSet<QString> commands = commandIds();
        QStringList problems;
        QSet<QString> ids;
        for (const Manual::Step &step : steps) {
            const QString what = where + " \"" + step.id + "\": ";
            if (step.id.isEmpty() || ids.contains(step.id))
                problems << what + "needs an id of its own";
            ids << step.id;
            problems << pointsAtWhatIsThere(what, step, names, commands) << wellFormed(what, step, feature);
        }
        return problems;
    }

private slots:
    void theTourAndWhatIsNewPointAtWhatIsThere() {
        const QSet<QString> names = objectNames();
        QVERIFY(names.contains("search") && names.contains("assistantPane"));
        QVERIFY(!Manual::features().isEmpty());
        const QStringList problems =
            stepProblems("tour.cpp", Manual::tourSteps(), false) + stepProblems("whatsnew.cpp", Manual::features(), true);
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }

    void theHelpPagesNameCommandsThereAre() {
        const QSet<QString> commands = commandIds();
        QStringList problems;
        QSet<QString> topics;
        for (const Manual::Topic &topic : Manual::topicList()) {
            const QString what = "helptopics.cpp \"" + topic.id + "\": ";
            if (topic.id.isEmpty() || topics.contains(topic.id) || topic.title.isEmpty() || topic.summary.isEmpty()
                || topic.sections.isEmpty())
                problems << what + "needs an id of its own, a title, a summary and a section";
            topics << topic.id;
            for (const Manual::Section &section : topic.sections) {
                for (const QString &command : Manual::mentioned(section.text))
                    if (!commands.contains(command))
                        problems << what + "names {" + command + "}, which is no command";
                if (Manual::mentioned(section.text).size() > section.text.count('{'))
                    problems << what + "a brace isn't a command";
            }
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
        QVERIFY(topics.contains("sharing"));
    }

    void theHelpIsTheSameTextInTheWindowAndATerminal() {
        // The pages are the same text for the window and the command line,
        // with the keys as they are bound: here, one of them moved.
        QTemporaryDir dir;
        QVERIFY(Fixtures::write(dir.filePath("keys.json"), "{\"library\": {\"Ctrl+N\": \"add\"}}"));
        Customization custom(dir.path());
        const Manual manual(nullptr, &custom, "0.1.0");
        const QString page = manual.topicText("start");
        QVERIFY2(page.contains("A adds book files") && page.contains("A  Ctrl+N"), qPrintable(page));
        QVERIFY(!page.contains('{'));
        QCOMPARE(manual.topicJson("start").value("title").toString(), QString("Getting started"));
        QVERIFY(manual.topicText("nothing").isEmpty());
        bool inWindow = false;
        for (const QVariant &topic : manual.topics())
            if (topic.toMap().value("id").toString() == "start")
                inWindow =
                    topic.toMap().value("sections").toList().first().toMap().value("html").toString().contains("<b>A</b> adds book files");
        QVERIFY(inWindow);
        // Every command is in the help, with a description to show.
        QCOMPARE(manual.commands().size(), Customization::commands().size());
        for (const QVariant &command : manual.commands())
            QVERIFY(!command.toMap().value("text").toString().isEmpty());
        // The page on sharing names its keys, and says what is never shared.
        const QString sharing = manual.topicText("sharing");
        QVERIFY2(sharing.contains("Shift+P publishes it") && sharing.contains("What is never shared"), qPrintable(sharing));
    }
};

QTEST_MAIN(ManualTests)
#include "manual_tests.moc"
