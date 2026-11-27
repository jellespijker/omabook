// A library on a disk with no room (#279): said to be full, with what to do,
// never only "disk I/O error"; nothing is removed that can't be recorded, and
// once there is room again the library is as it was. The disk is the small one
// in memory a test container has; where that is large, or can't be written,
// nothing is filled.

#include <QDir>
#include <QFile>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

#include "library.h"
#include "mcpharness.h"

using namespace Qt::StringLiterals;

namespace {

const QString smallDisk = u"/dev/shm"_s;

[[nodiscard]] bool fillable() {
    const QStorageInfo disk(smallDisk);
    return disk.isValid() && !disk.isReadOnly() && disk.bytesTotal() <= qint64{128} * 1024 * 1024;
}

// Writes `file` until the disk has no more than `spare` bytes left.
[[nodiscard]] bool fill(const QString &file, qint64 spare) {
    QFile filler(file);
    if (!filler.open(QIODevice::WriteOnly | QIODevice::Unbuffered))
        return false;
    for (qint64 block = 1024 * 1024; block >= 1024; block /= 2) {
        const QByteArray bytes(block, 'x');
        while (QStorageInfo(smallDisk).bytesAvailable() - block >= spare && filler.write(bytes) == block) {
        }
    }
    return QStorageInfo(smallDisk).bytesAvailable() <= spare + 4096;
}

} // namespace

class FullDiskTests : public QObject {
    Q_OBJECT

    std::unique_ptr<QTemporaryDir> m_disk;

private slots:
    void init() {
        if (!fillable())
            QSKIP("There is no small disk in memory to fill here.");
        m_disk = std::make_unique<QTemporaryDir>(smallDisk + "/omabook-full-XXXXXX");
        QVERIFY(m_disk->isValid());
    }
    void cleanup() { m_disk.reset(); }

    void aLibraryOnAFullDiskSaysSo() {
        McpHarness::Machine machine;
        machine.library = m_disk->filePath("Library");
        const QString story = machine.dir.filePath("story.txt");
        QVERIFY(Fixtures::write(story, "It was a dark night.\n"));
        QCOMPARE(machine.omabook({"add", story, "--library", machine.library}).code, 0);
        const QString filler = m_disk->filePath("filler");
        QVERIFY(fill(filler, 0));

        for (const QStringList &arguments : {QStringList{"list"}, QStringList{"remove", "1"}, QStringList{"show", "1"}}) {
            const auto ran = machine.omabook(arguments + QStringList{"--library", machine.library});
            QCOMPARE(ran.code, 1);
            QVERIFY2(ran.errors.contains(machine.library + " is full") && ran.errors.contains("Free up space"), qPrintable(ran.errors));
        }
        QVERIFY(QFile::remove(filler));
        const auto listed = machine.omabook({"list", "--library", machine.library});
        QVERIFY2(listed.code == 0 && listed.output.contains("story"), qPrintable(listed.errors));
        QCOMPARE(machine.omabook({"text", "1", "--library", machine.library}).code, 0);
    }

    void aFileThatOnlyPartlyFitsSaysThereIsNoRoom() {
        const QTemporaryDir dir;
        const QString large = dir.filePath("large.txt");
        QVERIFY(Fixtures::write(large, QByteArray("A long story. ").repeated(150000)));
        Library library(m_disk->filePath("Library"));
        QVERIFY(library.open());
        QVERIFY(fill(m_disk->filePath("filler"), qint64{512} * 1024));

        const Library::Added added = library.add(large);
        QVERIFY2(added.error.contains("no room for large.txt"), qPrintable(added.error));
        QVERIFY(library.books().isEmpty());
    }

    void aBookStaysWhileItsRemovalCantBeRecorded() {
        const QTemporaryDir dir;
        QVERIFY(Fixtures::write(dir.filePath("story.txt"), "It was a dark night.\n"));
        Library library(m_disk->filePath("Library"));
        QVERIFY(library.open());
        const qint64 id = library.add(dir.filePath("story.txt")).id;
        const QString file = library.formatPath(id, "txt");
        QVERIFY(fill(m_disk->filePath("filler"), 0));

        const auto removed = library.remove(id);
        QVERIFY(!removed);
        QVERIFY2(Fixtures::whyNot(removed).contains("is full"), qPrintable(Fixtures::whyNot(removed)));
        QVERIFY(QFile::exists(file));
    }
};

QTEST_MAIN(FullDiskTests)
#include "fulldisk_tests.moc"
