// A library whose database can only be read (#259): it is not opened to be
// used, so nothing seems to be kept that isn't; and what a library open
// already refuses to take is said, never silently lost.

#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QQuickStyle>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QtTest>

#include "desk.h"
#include "library.h"
#include "marginnotes.h"
#include "mcpharness.h"

using namespace Qt::StringLiterals;

namespace {

// Whether the file may be written by its owner.
void setWritable(const QString &file, bool writable) {
    const auto permissions = QFile::permissions(file);
    QFile::setPermissions(file, writable ? permissions | QFile::WriteOwner | QFile::WriteUser
                                         : permissions & ~(QFile::WriteOwner | QFile::WriteUser | QFile::WriteGroup));
}

// Makes the database refuse every new annotation, as a write that fails does.
[[nodiscard]] bool refuseAnnotations(const QString &database) {
    bool made = false;
    {
        QSqlDatabase other = QSqlDatabase::addDatabase("QSQLITE", u"refusing"_s);
        other.setDatabaseName(database);
        made =
            other.open()
            && QSqlQuery(other).exec(u"CREATE TRIGGER refusing BEFORE INSERT ON annotations BEGIN SELECT RAISE(ABORT, 'refused'); END"_s);
        other.close();
    }
    QSqlDatabase::removeDatabase(u"refusing"_s);
    return made;
}

} // namespace

class UnwritableTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void aDatabaseThatCanOnlyBeReadIsSaidToBe() {
        const McpHarness::Machine machine;
        const QString story = machine.dir.filePath("story.txt");
        const QString other = machine.dir.filePath("other.txt");
        QVERIFY(Fixtures::write(story, "It was a dark night.\n") && Fixtures::write(other, "Another night.\n"));
        QCOMPARE(machine.omabook({"add", story, "--library", machine.library}).code, 0);
        const QString database = Library::databaseIn(machine.library);
        setWritable(database, false);
        const auto back = qScopeGuard([&database] { setWritable(database, true); });
        if (QFileInfo(database).isWritable())
            QSKIP("Permissions don't hold back whoever runs this.");

        const QList<QStringList> commands{{"progress", "1", "--section", "0", "--percent", "50"},
                                          {"set", "1", "--title", "X"},
                                          {"remove", "1"},
                                          {"add", other},
                                          {"annotations", "all"},
                                          {"list"}};
        for (const QStringList &arguments : commands) {
            const auto ran = machine.omabook(arguments + QStringList{"--library", machine.library});
            QVERIFY2(ran.code == 1, qPrintable(arguments.join(' ') + u": "_s + ran.output + ran.errors));
            QVERIFY2(ran.errors.contains(machine.library + " is read-only") && ran.errors.contains("chmod"), qPrintable(ran.errors));
        }
        setWritable(database, true);
        const auto listed = machine.omabook({"list", "--library", machine.library});
        QVERIFY2(listed.code == 0 && listed.output.contains("story") && !listed.output.contains("other"), qPrintable(listed.output));
    }

    void aBookStaysWhenItsRemovalCantBeRecorded() {
        const QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        QVERIFY(Fixtures::write(dir.filePath("story.txt"), "It was a dark night.\n"));
        const qint64 id = library.add(dir.filePath("story.txt")).id;
        const QString file = library.formatPath(id, "txt");
        setWritable(library.databasePath(), false);
        const auto back = qScopeGuard([&library] { setWritable(library.databasePath(), true); });
        if (QFileInfo(library.databasePath()).isWritable())
            QSKIP("Permissions don't hold back whoever runs this.");

        const auto removed = library.remove(id);
        QVERIFY(!removed);
        QVERIFY2(Fixtures::whyNot(removed).contains("read-only"), qPrintable(Fixtures::whyNot(removed)));
        QVERIFY(QFileInfo::exists(file) && library.book(id));
    }

    void aBookmarkOrNoteThatIsNotKeptIsSaid() {
        Desk desk("en", {}, false);
        const QString file = desk.dir.filePath("The Ferry.txt");
        QVERIFY(Fixtures::write(file, "She watched the lantern on the far pier until it went out.\n"));
        Library &library = *desk.backend->library();
        const qint64 book = library.add(file).id;
        desk.reader->openBook(book);
        QTRY_VERIFY(desk.reader->isOpen());
        QVERIFY(refuseAnnotations(library.databasePath()));

        QVERIFY(!desk.reader->addBookmark("here"));
        QVERIFY2(desk.backend->message().contains("can't be written"), qPrintable(desk.backend->message()));
        desk.backend->dismissMessage();

        MarginNotes notes;
        notes.setReader(&*desk.reader);
        const QString shown = desk.reader->sectionText();
        const qsizetype start = shown.indexOf("lantern");
        QGuiApplication::clipboard()->clear();
        QVERIFY(!notes.write(shown, start, start + 7, "Typed with care."));
        QCOMPARE(QGuiApplication::clipboard()->text(), u"Typed with care."_s);
        QVERIFY2(desk.backend->message().contains("clipboard"), qPrintable(desk.backend->message()));
    }
};

QTEST_MAIN(UnwritableTests)
#include "unwritable_tests.moc"
