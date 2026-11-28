// A dry run's plain text names everything a sync would carry that it counts:
// bookmarks and highlights made or removed on either side, and collections,
// and says both libraries are in step only when none of it differs (#293).

#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include "bookmeta.h"
#include "fixtures.h"
#include "library.h"
#include "remote.h"
#include "sync.h"

using namespace Qt::StringLiterals;

namespace {

// This computer and another, "laptop", behind a stand-in ssh, in step with one book.
struct Computers {
    QTemporaryDir dir;
    Library here{dir.filePath(u"Here"_s)};
    Library there{dir.filePath(u"There"_s)};
    QString ssh = Fixtures::standInSsh(dir.path(), QString::fromLatin1(OMABOOK_BINARY));

    [[nodiscard]] bool made() {
        const QString file = dir.filePath(u"salt.epub"_s);
        return here.open() && there.open() && Fixtures::epub(file, u"The Salt Road"_s) && here.add(file).id > 0 && synced();
    }

    [[nodiscard]] bool synced() { return syncLibrary(here, Remote(u"laptop:"_s + there.root(), {ssh})).error.isEmpty(); }

    // What `omabook sync --dry-run` prints.
    [[nodiscard]] QString dryRun() const {
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(u"PATH"_s, dir.path() + u':' + environment.value(u"PATH"_s));
        environment.insert(u"QT_QPA_PLATFORM"_s, u"offscreen"_s);
        QProcess process;
        process.setProcessEnvironment(environment);
        process.start(QString::fromLatin1(OMABOOK_BINARY),
                      {u"sync"_s, u"--dry-run"_s, u"--library"_s, here.root(), u"laptop:"_s + there.root()});
        process.closeWriteChannel();
        process.waitForFinished(120000);
        return QString::fromUtf8(process.readAllStandardOutput() + process.readAllStandardError());
    }
};

} // namespace

class DryRunSaidTests : public QObject {
    Q_OBJECT

private slots:
    void bookmarksMadeAndRemovedAreSaid() {
        Computers computers;
        QVERIFY(computers.made());
        QVERIFY(computers.dryRun().contains(u"in step"_s));
        Annotation bookmark;
        bookmark.kind = u"bookmark"_s;
        const qint64 stored = computers.here.addAnnotation(computers.here.books().first().id, bookmark);
        QVERIFY(stored > 0);
        QString said = computers.dryRun();
        QVERIFY2(said.contains(u"Bookmarks and highlights updated on laptop: 1"_s) && !said.contains(u"in step"_s), qPrintable(said));
        QVERIFY(computers.synced());
        QVERIFY(computers.dryRun().contains(u"in step"_s));
        QTest::qWait(1100); // times are kept to the second
        computers.here.removeAnnotation(stored);
        said = computers.dryRun();
        QVERIFY2(said.contains(u"Bookmarks and highlights updated on laptop: 1"_s) && !said.contains(u"in step"_s), qPrintable(said));
    }

    void aCollectionMadeThereIsSaid() {
        Computers computers;
        QVERIFY(computers.made());
        QVERIFY(computers.there.addCollection(u"Shelf"_s).has_value());
        const QString said = computers.dryRun();
        QVERIFY2(said.contains(u"Collections updated here: 1"_s) && !said.contains(u"in step"_s), qPrintable(said));
    }
};

QTEST_GUILESS_MAIN(DryRunSaidTests)
#include "dryrunsaid_tests.moc"
