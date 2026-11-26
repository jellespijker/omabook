// A library that can only be read, or whose database is damaged (#227): the
// reason and what to do are said, never that another program is setting it up,
// nor that its books don't exist.

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QtTest>

#include "library.h"
#include "mcpharness.h"

using namespace Qt::StringLiterals;

namespace {

// A machine with a library of one book, made by the command.
[[nodiscard]] bool oneBook(const McpHarness::Machine &machine) {
    const QString story = machine.dir.filePath("story.txt");
    return Fixtures::write(story, "It was a dark night.\n") && machine.omabook({"add", story, "--library", machine.library}).code == 0;
}

// Whether the folder, and all in it, may be written by its owner.
void setWritable(const QString &folder, bool writable) {
    QStringList paths{folder};
    QDirIterator walk(folder, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (walk.hasNext())
        paths << walk.next();
    for (const QString &path : std::as_const(paths)) {
        const auto permissions = QFile::permissions(path);
        QFile::setPermissions(path, writable ? permissions | QFile::WriteOwner | QFile::WriteUser
                                             : permissions & ~(QFile::WriteOwner | QFile::WriteUser | QFile::WriteGroup));
    }
}

// Random bytes over the start of the database, as a failing disk may leave it.
[[nodiscard]] bool damage(const QString &database) {
    QFile file(database);
    QByteArray noise(100, '\0');
    for (qsizetype at = 0; at < noise.size(); ++at)
        noise[at] = static_cast<char>(at * 151 + 7);
    return file.open(QIODevice::ReadWrite) && file.write(noise) == noise.size();
}

const QList<QStringList> asking{{"list"}, {"show", "1"}, {"text", "1"}, {"toc", "1"}};

} // namespace

class LibraryUnusableTests : public QObject {
    Q_OBJECT

private slots:
    void aReadOnlyLibrarySaysSo() {
        const McpHarness::Machine machine;
        QVERIFY(oneBook(machine));
        setWritable(machine.library, false);
        const auto back = qScopeGuard([&machine] { setWritable(machine.library, true); });
        if (QFileInfo(machine.library).isWritable())
            QSKIP("Permissions don't hold back whoever runs this.");

        for (const QStringList &arguments : asking) {
            const auto ran = machine.omabook(arguments + QStringList{"--library", machine.library});
            QCOMPARE(ran.code, 1);
            QVERIFY2(ran.errors.contains(machine.library + " is read-only") && ran.errors.contains("chmod"), qPrintable(ran.errors));
            QVERIFY2(!ran.errors.contains("another program") && !ran.errors.contains("no book"), qPrintable(ran.errors));
        }
        Library library(machine.library);
        QString why;
        QVERIFY(!library.open(&why));
        QVERIFY2(why.contains("read-only"), qPrintable(why));
    }

    void aDamagedDatabaseSaysSo() {
        const McpHarness::Machine machine;
        QVERIFY(oneBook(machine));
        const QString database = Library::databaseIn(machine.library);
        QVERIFY(damage(database));

        for (const QStringList &arguments : asking) {
            const auto ran = machine.omabook(arguments + QStringList{"--library", machine.library});
            QCOMPARE(ran.code, 1);
            QVERIFY2(ran.errors.contains(database + " is damaged"), qPrintable(ran.errors));
            QVERIFY2(ran.errors.contains(machine.library + "/.backups") && ran.errors.contains("snapshot restore"), qPrintable(ran.errors));
            QVERIFY2(!ran.errors.contains("another program") && !ran.errors.contains("no book"), qPrintable(ran.errors));
        }
    }

    // A book's number in a library that opens is still said to be no book of it.
    void aBookThatIsNotThereIsStillSaid() {
        const McpHarness::Machine machine;
        QVERIFY(oneBook(machine));
        const auto ran = machine.omabook({"text", "7", "--library", machine.library});
        QCOMPARE(ran.code, 1);
        QVERIFY2(ran.errors.contains("There is no book 7"), qPrintable(ran.errors));
    }
};

QTEST_MAIN(LibraryUnusableTests)
#include "libraryunusable_tests.moc"
