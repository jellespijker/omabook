#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include "fixtures.h"
#include "folio.h"
#include "library.h"
#include "settings.h"

// Omabook's answers to Folio, the file manager, which runs its commands and
// shows what they print.
class FolioTests : public QObject {
    Q_OBJECT

    QTemporaryDir m_home;

private slots:
    // The default library is in the home folder; the tests get one of their
    // own, so the real one is never looked at.
    void initTestCase() { qputenv("HOME", m_home.path().toUtf8()); }

    void theLibrariesAreItemsThatOpenInOmabook() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        settings.setLibraryPath(dir.filePath("Novels"));
        // A library that isn't there yet is not made by being looked at.
        QCOMPARE(Folio::libraries(settings).value("items").toArray().size(), 0);
        QVERIFY(!QFileInfo::exists(dir.filePath("Novels")));

        QVERIFY(Fixtures::epub(dir.filePath("one.epub")));
        QCOMPARE(Folio::add(settings, {}, {dir.filePath("one.epub")}).value("notice").toArray().first().toString(),
                 QString("Added %1 to %2."));
        QCOMPARE(Folio::add(settings, {}, {dir.filePath("one.epub")}).value("notice").toArray().first().toString(),
                 QString("Already in %1."));
        QVERIFY(Folio::add(settings, {}, {dir.filePath("nothing.txt")}).contains("error"));

        const QJsonArray items = Folio::libraries(settings).value("items").toArray();
        QCOMPARE(items.size(), 1);
        const QJsonObject item = items.first().toObject();
        QCOMPARE(item.value("id").toString(), dir.filePath("Novels"));
        QCOMPARE(item.value("name").toString(), QString("Novels"));
        QCOMPARE(item.value("subtitle").toArray(), (QJsonArray{"%1 books", "1"}));
        QCOMPARE(item.value("open").toObject().value("run").toArray().last().toString(), dir.filePath("Novels"));
    }

    void switchedOffItShowsNothing() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        settings.setLibraryPath(dir.filePath("Novels"));
        QVERIFY(Fixtures::epub(dir.filePath("one.epub")));
        QVERIFY(!Folio::add(settings, {}, {dir.filePath("one.epub")}).contains("error"));
        QCOMPARE(Folio::libraries(settings).value("items").toArray().size(), 1);
        // Switched off in Omabook, the commands Folio runs answer with
        // nothing to show and nothing to offer.
        settings.setFolio(false);
        QVERIFY(!settings.folio());
        QProcess folio;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert("XDG_CONFIG_HOME", dir.filePath("config"));
        QDir().mkpath(dir.filePath("config/omabook"));
        QFile::copy(dir.filePath("config.json"), dir.filePath("config/omabook/config.json"));
        folio.setProcessEnvironment(environment);
        folio.start(QStringLiteral(OMABOOK_BINARY), {"folio", "libraries"});
        QVERIFY(folio.waitForFinished(20000));
        const QJsonObject answer = QJsonDocument::fromJson(folio.readAllStandardOutput()).object();
        QVERIFY(answer.value("items").toArray().isEmpty());
        QVERIFY(answer.value("actions").isArray() && answer.value("actions").toArray().isEmpty());
    }

    void aBookFileSaysWhereItStands() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        settings.setLibraryPath(dir.filePath("Novels"));
        QVERIFY(Fixtures::epub(dir.filePath("one.epub")));
        QVERIFY(Fixtures::epub(dir.filePath("other.epub"), "Another Book"));
        QVERIFY(!Folio::add(settings, {}, {dir.filePath("one.epub")}).contains("error"));
        qint64 id = 0;
        {
            Library library(dir.filePath("Novels"));
            QVERIFY(library.open());
            id = library.books().first().id;
            library.setProgress(id, {1, 0.5, 40, 0});
            library.setReading(id, "", 4, {});
        }
        const QJsonArray rows = Folio::details(settings, dir.filePath("one.epub"));
        QCOMPARE(rows.size(), 3);
        QCOMPARE(rows.at(0).toArray(), (QJsonArray{"Omabook", "Novels"}));
        QCOMPARE(rows.at(1).toArray(), (QJsonArray{"Read", "40%"}));
        QCOMPARE(rows.at(2).toArray().last().toString(), QString("★★★★"));
        // A book no library has, and a file that is no book, say nothing.
        QVERIFY(Folio::details(settings, dir.filePath("other.epub")).isEmpty());
        QVERIFY(Folio::details(settings, dir.filePath("config.json")).isEmpty());

        // What is being read is listed, and opens by its number.
        const QJsonArray reading = Folio::reading(settings).value("items").toArray();
        QCOMPARE(reading.size(), 1);
        QCOMPARE(reading.first().toObject().value("id").toString(), QString::number(id));
        QCOMPARE(reading.first().toObject().value("open").toObject().value("run").toArray(),
                 (QJsonArray{"omabook", "folio", "open", QString::number(id)}));
    }

    void thePluginsAreWhatFolioExpects() {
        const QDir plugins(QStringLiteral(OMABOOK_SOURCE) + "/share/folio");
        for (const QString &plugin : {QString("omabook"), QString("omabook-reading")}) {
            QFile file(plugins.filePath(plugin + "/manifest.json"));
            QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
            const QJsonObject manifest = QJsonDocument::fromJson(file.readAll()).object();
            QCOMPARE(manifest.value("folio").toInt(), 1);
            QCOMPARE(manifest.value("poll").toObject().value("command").toArray().at(1).toString(), QString("folio"));
            QVERIFY(QFileInfo::exists(plugins.filePath(plugin + "/omabook.svg")));
            // Every catalog translates what the English one lists.
            QFile english(plugins.filePath(plugin + "/i18n/en.json"));
            QVERIFY(english.open(QIODevice::ReadOnly));
            const QStringList texts = QJsonDocument::fromJson(english.readAll()).object().keys();
            QVERIFY(texts.contains(manifest.value("section").toObject().value("title").toString()));
            for (const QString &code : {QString("nl"), QString("de"), QString("fr"), QString("es-MX")}) {
                QFile catalog(plugins.filePath(plugin + "/i18n/" + code + ".json"));
                QVERIFY2(catalog.open(QIODevice::ReadOnly), qPrintable(catalog.fileName()));
                QCOMPARE(QJsonDocument::fromJson(catalog.readAll()).object().keys(), texts);
            }
        }
    }
};

QTEST_MAIN(FolioTests)
#include "folio_tests.moc"
