// Snapshots kept by a lost computer, opened on another with the recovery key
// (#292): listed, restored into a new folder and never thinned there; a wrong
// key, or none, changes nothing, and a password kept already is never written
// over. In folders and repositories of the test's own, never the user's.

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include "bookmeta.h"
#include "fixtures.h"
#include "library.h"
#include "snapshotrestore.h"
#include "snapshots.h"

using Fixtures::whyNot;
using namespace Qt::StringLiterals;

namespace {

// A lost computer's snapshot in a repository on "a disk", and the new
// computer's own state beside it.
struct Disk {
    QTemporaryDir dir;
    QString repository = dir.filePath(u"disk/repository"_s);
    QString lostLibrary = dir.filePath(u"lost/OmaBooks"_s);
    QString key;

    [[nodiscard]] Snapshots computer(const QString &name) const {
        return Snapshots(SnapshotPlace::in(dir.filePath(name + u"/state"_s)),
                         {.settingsFile = dir.filePath(name + u"/config.json"_s), .pluginsFolder = dir.filePath(name + u"/plugins"_s)});
    }
    [[nodiscard]] QString password(const QString &name) const { return dir.filePath(name + u"/state/password"_s); }

    [[nodiscard]] bool made() {
        Library library(lostLibrary);
        BookMeta meta;
        meta.title = u"The Salt Road"_s;
        if (!library.open() || library.addEntry(meta, true).id <= 0)
            return false;
        Snapshots snapshots = computer(u"lost"_s);
        const auto setUp = snapshots.setUp(repository);
        if (!setUp)
            return false;
        key = *setUp;
        return snapshots.take(library, SnapshotReason::Asked).has_value();
    }
};

[[nodiscard]] std::optional<Failure::Code> refused(const std::expected<QString, Failure> &done) {
    return done ? std::nullopt : std::optional(done.error().code());
}

} // namespace

class SnapshotOpenTests : public QObject {
    Q_OBJECT

private slots:
    void aLostComputersSnapshotsOpenWithTheirRecoveryKey() {
        Disk disk;
        QVERIFY(disk.made());
        Snapshots fresh = disk.computer(u"new"_s);
        QVERIFY(refused(fresh.setUp(disk.repository)) == Failure::Code::SnapshotRepositoryExists);
        QVERIFY(refused(fresh.setUp(disk.repository, Snapshots::newRecoveryKey())) == Failure::Code::SnapshotKeyRefused);
        QVERIFY(!fresh.ready() && !QFile::exists(disk.password(u"new"_s)));

        // The key as a person types it: lower case, no dashes.
        const auto opened = fresh.setUp(disk.repository, disk.key.toLower().remove(u'-'));
        QVERIFY2(opened.has_value(), qPrintable(whyNot(opened)));
        QCOMPARE(*opened, disk.key);
        Snapshots again = disk.computer(u"new"_s);
        QVERIFY(again.ready() && again.place().adopted.isValid());

        Library here(disk.dir.filePath(u"new/OmaBooks"_s));
        QVERIFY(here.open());
        const auto others = again.elsewhere(here);
        QVERIFY2(others.has_value(), qPrintable(whyNot(others)));
        QCOMPARE(others->size(), 1);
        QCOMPARE(others->first().library, disk.lostLibrary);
        const Library lost(disk.lostLibrary);
        const auto listed = again.list(lost);
        QVERIFY(listed && listed->size() == 1);
        const QString restored = disk.dir.filePath(u"restored"_s);
        const auto whole = SnapshotRestore::whole(again, listed->first(), restored);
        QVERIFY2(whole.has_value(), qPrintable(whyNot(whole)));
        Library back(restored);
        QVERIFY(back.open());
        QCOMPARE(back.books().size(), 1);

        // What was there when it was opened is thinned by no later night.
        QCOMPARE(again.thin(lost, QDateTime::currentDateTime().addYears(3)).value_or(-1), 0);
    }

    // The password gone and the choices left: said so, nothing made over them,
    // and put back with the recovery key (#296).
    void aPasswordGoneIsPutBackWithTheRecoveryKey() {
        Disk disk;
        QVERIFY(disk.made());
        QVERIFY(QFile::remove(disk.password(u"lost"_s)));
        Snapshots snapshots = disk.computer(u"lost"_s);
        QVERIFY(!snapshots.ready());
        QVERIFY(refused(snapshots.setUp()) == Failure::Code::SnapshotPasswordMissing);
        QVERIFY(!QFile::exists(disk.password(u"lost"_s)));
        const auto opened = snapshots.setUp({}, disk.key);
        QVERIFY2(opened.has_value(), qPrintable(whyNot(opened)));
        Snapshots again = disk.computer(u"lost"_s);
        QVERIFY(again.ready() && !again.place().adopted.isValid());
        QCOMPARE(again.place().repository.location, disk.repository);
    }

    void aPasswordKeptAlreadyIsNeverWrittenOver() {
        Disk disk;
        QVERIFY(disk.made());
        QVERIFY(QDir().mkpath(disk.dir.filePath(u"new/state"_s)));
        QVERIFY(Fixtures::write(disk.password(u"new"_s), "another-password"));
        Snapshots fresh = disk.computer(u"new"_s);
        QVERIFY(refused(fresh.setUp(disk.repository, disk.key)) == Failure::Code::SnapshotPasswordOther);
        QFile kept(disk.password(u"new"_s));
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), QByteArray("another-password"));
        QVERIFY(!fresh.ready());
    }

    void theCommandLineOpensThemAndNamesTheirLibraries() {
        Disk disk;
        QVERIFY(disk.made());
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        for (const auto &[name, folder] : {std::pair{"HOME", "home"},
                                           {"XDG_CONFIG_HOME", "home/.config"},
                                           {"XDG_CACHE_HOME", "home/.cache"},
                                           {"XDG_DATA_HOME", "home/.local/share"},
                                           {"XDG_STATE_HOME", "home/.local/state"}})
            environment.insert(QString::fromLatin1(name), disk.dir.filePath(QString::fromLatin1(folder)));
        environment.insert(u"QT_QPA_PLATFORM"_s, u"offscreen"_s);
        environment.remove(u"CREDENTIALS_DIRECTORY"_s);
        const auto omabook = [&environment](const QStringList &arguments, const QByteArray &typed = {}) {
            QProcess process;
            process.setProcessEnvironment(environment);
            process.start(QString::fromLatin1(OMABOOK_BINARY), arguments);
            process.write(typed);
            process.closeWriteChannel();
            process.waitForFinished(120000);
            return std::pair{process.exitCode(), QString::fromUtf8(process.readAllStandardOutput() + process.readAllStandardError())};
        };
        const auto [opened, said] =
            omabook({u"snapshot"_s, u"setup"_s, u"--repository"_s, disk.repository, u"--recovery-key"_s}, disk.key.toUtf8() + '\n');
        QVERIFY2(opened == 0 && said.contains(u"are opened"_s) && !said.contains(disk.key), qPrintable(said));
        const QString here = disk.dir.filePath(u"new/OmaBooks"_s);
        QCOMPARE(omabook({u"list"_s, u"--library"_s, here}).first, 0);
        const auto [listed, list] = omabook({u"snapshot"_s, u"list"_s, u"--library"_s, here});
        QVERIFY2(listed == 0 && list.contains(disk.lostLibrary), qPrintable(list));
        const auto [theirs, json] = omabook({u"snapshot"_s, u"list"_s, u"--library"_s, disk.lostLibrary, u"--json"_s});
        QVERIFY2(theirs == 0, qPrintable(json));
        QCOMPARE(QJsonDocument::fromJson(json.toUtf8()).object().value("snapshots"_L1).toArray().size(), 1);
    }
};

QTEST_GUILESS_MAIN(SnapshotOpenTests)
#include "snapshotopen_tests.moc"
