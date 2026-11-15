#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>
#include <sys/stat.h>

#include "fixtures.h"
#include "folio.h"
#include "library.h"
#include "settings.h"

// Omabook's answers to Folio, the file manager, which runs its commands and
// shows what they print, and the views of the libraries it shows as folders.
class FolioTests : public QObject {
    Q_OBJECT

    // The views are in the home folder and the trash in the data folder; the
    // tests get both of their own, so the real ones are never looked at.
    QTemporaryDir m_home{scratch()};

    // On the disk the build is on: a trash in a memory filesystem (/tmp)
    // takes nothing, and the books must go to one the tests can look in.
    static QString scratch() { return QStringLiteral(OMABOOK_TEST_TMP) + "/folio-tests-XXXXXX"; }

    static qint64 inode(const QString &path) {
        struct stat info {};
        return ::stat(QFile::encodeName(path).constData(), &info) == 0 ? qint64(info.st_ino) : 0;
    }
    // Long enough for a file copied in to count as settled.
    static void settle() { QThread::msleep(2200); }
    static QStringList listing(const QString &folder) { return QDir(folder).entryList(QDir::Files | QDir::System | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name); }
    static QString nameOf(const Book &book) { return book.meta.title + " - " + book.meta.authorText() + ".epub"; }

    // A library with books of these titles, its settings, and the books.
    struct Shelf {
        QTemporaryDir dir{scratch()};
        Settings settings{dir.filePath("config.json")};
        QString library = dir.filePath("Novels");
        explicit Shelf(const QStringList &titles) {
            settings.setLibraryPath(library);
            Library books(library);
            books.open();
            for (const QString &title : titles) {
                const QString file = dir.filePath(title + ".epub");
                Fixtures::epub(file, title);
                books.add(file);
            }
        }
        QList<Book> books() const {
            Library books(library);
            books.open();
            return books.books();
        }
        QString view() const { return Folio::viewFolder(library); }
        // The real binary, as Folio runs it, with this shelf's settings.
        QJsonDocument run(const QStringList &arguments) const {
            QDir().mkpath(dir.filePath("config/omabook"));
            QFile::remove(dir.filePath("config/omabook/config.json"));
            QFile::copy(dir.filePath("config.json"), dir.filePath("config/omabook/config.json"));
            QProcess omabook;
            QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
            environment.insert("XDG_CONFIG_HOME", dir.filePath("config"));
            omabook.setProcessEnvironment(environment);
            omabook.start(QStringLiteral(OMABOOK_BINARY), arguments);
            omabook.waitForFinished(30000);
            return QJsonDocument::fromJson(omabook.readAllStandardOutput());
        }
    };

private slots:
    void initTestCase() {
        qputenv("HOME", m_home.path().toUtf8());
        qputenv("XDG_DATA_HOME", m_home.filePath(".local/share").toUtf8());
        qputenv("XDG_CONFIG_HOME", m_home.filePath(".config").toUtf8());
        qputenv("XDG_CACHE_HOME", m_home.filePath(".cache").toUtf8());
        QVERIFY(Folio::viewsRoot().startsWith(m_home.path()));
        // Folio makes the trash when it first sends something there; the
        // tests send files there as Folio does, through Qt, which doesn't.
        QVERIFY(QDir().mkpath(m_home.filePath(".local/share/Trash/files")));
        QVERIFY(QDir().mkpath(m_home.filePath(".local/share/Trash/info")));
    }

    void theLibrariesAreFoldersOfTheirBooks() {
        QTemporaryDir dir(scratch());
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
        // Opening it goes to its view, in Folio, not to Omabook.
        const QString view = item.value("open").toObject().value("path").toString();
        QCOMPARE(view, Folio::viewFolder(dir.filePath("Novels")));
        QVERIFY(view.startsWith(Folio::viewsRoot() + "/"));
        QVERIFY(!item.value("open").toObject().contains("run"));

        // One entry, by its title and author: the book's own file.
        Library library(dir.filePath("Novels"));
        QVERIFY(library.open());
        const Book book = library.books().first();
        QCOMPARE(listing(view), QStringList{nameOf(book)});
        QCOMPARE(inode(view + "/" + nameOf(book)), inode(library.formatPath(book.id)));
        // Found by where it is, without reading it.
        const auto found = Folio::find(settings, view + "/" + nameOf(book));
        QVERIFY(found);
        QCOMPARE(found->id, book.id);
        QCOMPARE(Folio::find(settings, library.formatPath(book.id))->id, book.id);
        QVERIFY(!Folio::find(settings, dir.filePath("one.epub")));
    }

    void theViewFollowsTheLibrary() {
        Shelf shelf({"First Book", "Second Book"});
        QList<Book> books = shelf.books();
        QCOMPARE(Folio::sync(shelf.library).linked, 2);
        QCOMPARE(listing(shelf.view()), (QStringList{nameOf(books.at(0)), nameOf(books.at(1))}));
        // Nothing changed: nothing to do.
        const Folio::Synced again = Folio::sync(shelf.library);
        QCOMPARE(again.linked + again.imported + again.removed, 0);

        // New details, a new name; the library's folder was renamed with
        // them, and the entry still is the book's file.
        {
            Library library(shelf.library);
            QVERIFY(library.open());
            BookMeta meta = books.at(0).meta;
            meta.title = "Renamed: A/B";
            QVERIFY(library.setMeta(books.at(0).id, meta));
        }
        Folio::sync(shelf.library);
        books = shelf.books();
        const QString renamed = "Renamed: A-B - " + books.at(0).meta.authorText() + ".epub";
        QVERIFY2(listing(shelf.view()).contains(renamed), qPrintable(listing(shelf.view()).join(", ")));
        QCOMPARE(listing(shelf.view()).size(), 2);
        {
            Library library(shelf.library);
            QVERIFY(library.open());
            QCOMPARE(inode(shelf.view() + "/" + renamed), inode(library.formatPath(books.at(0).id)));
        }

        // A rename in Folio changes nothing in the library, and the entry
        // gets its name back.
        QVERIFY(QFile::rename(shelf.view() + "/" + renamed, shelf.view() + "/mine.epub"));
        const Folio::Synced back = Folio::sync(shelf.library);
        QCOMPARE(back.removed, 0);
        QCOMPARE(back.imported, 0);
        QVERIFY(listing(shelf.view()).contains(renamed));
        QVERIFY(!listing(shelf.view()).contains("mine.epub"));
        QCOMPARE(shelf.books().size(), 2);
        QCOMPARE(shelf.books().at(0).meta.title, QString("Renamed: A/B"));

        // A book removed in Omabook leaves the view.
        {
            Library library(shelf.library);
            QVERIFY(library.open());
            QVERIFY(library.remove(books.at(1).id));
        }
        Folio::sync(shelf.library);
        QCOMPARE(listing(shelf.view()), QStringList{renamed});
    }

    void aBookCopiedInIsAddedOnce() {
        Shelf shelf({"First Book"});
        Folio::sync(shelf.library);
        const QString view = shelf.view();

        // Half copied: a file that is still growing isn't added.
        QFile partial(view + "/Copied.epub");
        QVERIFY(Fixtures::epub(shelf.dir.filePath("whole.epub"), "Copied Book"));
        QFile whole(shelf.dir.filePath("whole.epub"));
        QVERIFY(whole.open(QIODevice::ReadOnly));
        const QByteArray bytes = whole.readAll();
        QVERIFY(partial.open(QIODevice::WriteOnly));
        partial.write(bytes.left(bytes.size() / 2));
        partial.flush();
        Folio::Synced synced = Folio::sync(shelf.library);
        QVERIFY(synced.pending);
        QCOMPARE(synced.imported, 0);
        settle();
        partial.write(bytes.mid(bytes.size() / 2));
        partial.close();
        synced = Folio::sync(shelf.library);
        QVERIFY(synced.pending);
        QCOMPARE(synced.imported, 0);
        QCOMPARE(shelf.books().size(), 1);

        // A file that isn't a book is left as it is, and said.
        QVERIFY(Fixtures::write(view + "/notes.jpg", "not a book"));
        settle();
        // The poll leaves the adding to a process of its own.
        synced = Folio::sync(shelf.library, Folio::Imports::Defer);
        QVERIFY(synced.importable);
        QCOMPARE(synced.imported, 0);
        synced = Folio::sync(shelf.library);
        QCOMPARE(synced.imported, 1);
        QCOMPARE(synced.ignored, QStringList{"notes.jpg: not a book"});
        QVERIFY(QFileInfo::exists(view + "/notes.jpg"));
        QList<Book> books = shelf.books();
        QCOMPARE(books.size(), 2);
        QCOMPARE(books.at(1).meta.title, QString("Copied Book"));
        // The dropped file gave way to the book's entry.
        QCOMPARE(listing(view), (QStringList{nameOf(books.at(1)), nameOf(books.at(0)), "notes.jpg"}));
        {
            Library library(shelf.library);
            QVERIFY(library.open());
            QCOMPARE(inode(view + "/" + nameOf(books.at(1))), inode(library.formatPath(books.at(1).id)));
        }
        const QJsonObject item = Folio::libraries(shelf.settings).value("items").toArray().first().toObject();
        QCOMPARE(item.value("subtitle").toArray(), (QJsonArray{"%1 books, %2 not added", "2", "1"}));

        // The same book again, under another name, is the book already there.
        QVERIFY(QFile::copy(shelf.dir.filePath("whole.epub"), view + "/again.epub"));
        Folio::sync(shelf.library);
        settle();
        synced = Folio::sync(shelf.library);
        QCOMPARE(synced.imported, 1);
        QCOMPARE(shelf.books().size(), 2);
        QVERIFY(!QFileInfo::exists(view + "/again.epub"));
    }

    void aDeletedEntrySendsItsBookToTheTrash() {
        Shelf shelf({"First Book", "Second Book", "Third Book"});
        Folio::sync(shelf.library);
        const QString view = shelf.view();
        QList<Book> books = shelf.books();
        {
            Library library(shelf.library);
            QVERIFY(library.open());
            library.setProgress(books.at(0).id, {2, 0.5, 40, 0});
            library.setReading(books.at(0).id, "reading", 4, {});
        }

        // Deleted in Folio, which sends it to the trash.
        QString trashedEntry;
        QVERIFY(QFile::moveToTrash(view + "/" + nameOf(books.at(0)), &trashedEntry));
        QCOMPARE(Folio::sync(shelf.library).removed, 1);
        QCOMPARE(shelf.books().size(), 2);
        // The book's folder is in the trash, whole.
        QFile state(Folio::viewsRoot() + "/." + QFileInfo(view).fileName() + ".json");
        QVERIFY(state.open(QIODevice::ReadOnly));
        const QJsonObject removed = QJsonDocument::fromJson(state.readAll()).object().value("removed").toObject();
        QCOMPARE(removed.size(), 1);
        const QString trashed = removed.begin()->toObject().value("trashed").toString();
        QVERIFY2(QFileInfo(trashed).isDir() && !QDir(trashed).entryList({"*.epub"}).isEmpty(), qPrintable(trashed));

        // Brought back from the trash, it is the book again, as it was.
        QVERIFY(QFile::rename(trashedEntry, view + "/" + nameOf(books.at(0))));
        Folio::sync(shelf.library);
        settle();
        QCOMPARE(Folio::sync(shelf.library).imported, 1);
        books = shelf.books();
        QCOMPARE(books.size(), 3);
        const Book back = books.last();
        QCOMPARE(back.meta.title, QString("First Book"));
        QCOMPARE(back.rating, 4);
        QCOMPARE(back.status(), QString("reading"));
        QCOMPARE(qRound(back.percent), 40);

        // A view emptied all at once, or gone, removes nothing.
        for (const QString &name : listing(view))
            QVERIFY(QFile::remove(view + "/" + name));
        QCOMPARE(Folio::sync(shelf.library).removed, 0);
        QCOMPARE(shelf.books().size(), 3);
        QCOMPARE(listing(view).size(), 3);
        QVERIFY(QDir(view).removeRecursively());
        QCOMPARE(Folio::sync(shelf.library).removed, 0);
        QCOMPARE(shelf.books().size(), 3);
        QCOMPARE(listing(view).size(), 3);

        // A library that can't be reached leaves its view alone.
        QVERIFY(QDir().rename(shelf.library, shelf.dir.filePath("Unplugged")));
        QVERIFY(QFile::remove(view + "/" + listing(view).first()));
        Folio::sync(shelf.library);
        QCOMPARE(listing(view).size(), 2);
        QVERIFY(QDir().rename(shelf.dir.filePath("Unplugged"), shelf.library));
        // Back again, the entry deleted meanwhile counts as deleted.
        QCOMPARE(Folio::sync(shelf.library).removed, 1);
        QCOMPARE(shelf.books().size(), 2);
    }

    void theMenuAndTheDetailsGoThroughTheCommandLine() {
        Shelf shelf({"First Book", "Second Book"});
        // The poll, as Folio runs it, makes the view.
        const QJsonObject poll = shelf.run({"folio", "libraries"}).object();
        QCOMPARE(poll.value("items").toArray().first().toObject().value("open").toObject().value("path").toString(), shelf.view());
        QList<Book> books = shelf.books();
        const QString entry = shelf.view() + "/" + nameOf(books.at(0));
        QVERIFY(QFileInfo::exists(entry));

        const QJsonArray rows = shelf.run({"folio", "details", entry}).array();
        QCOMPARE(rows.at(0).toArray(), (QJsonArray{"Title", "First Book"}));
        QVERIFY(rows.contains(QJsonArray{"Formats", "EPUB"}));
        QVERIFY(rows.contains(QJsonArray{"Omabook", "Novels"}));

        QCOMPARE(shelf.run({"folio", "mark", "read", entry}).object().value("notice").toArray(),
                 (QJsonArray{"Marked %1 as read.", "First Book"}));
        QCOMPARE(shelf.books().at(0).status(), QString("read"));
        QVERIFY(!shelf.books().at(0).finished.isEmpty());
        shelf.run({"folio", "mark", "reading", entry});
        QCOMPARE(shelf.books().at(0).status(), QString("reading"));

        // Removing goes to the trash, the entry with it.
        const QDir trash(m_home.filePath(".local/share/Trash/files"));
        const qsizetype inTrash = trash.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size();
        QCOMPARE(shelf.run({"folio", "remove", entry}).object().value("notice").toArray(),
                 (QJsonArray{"Moved %1 to the trash.", "First Book"}));
        QCOMPARE(shelf.books().size(), 1);
        QVERIFY(!QFileInfo::exists(entry));
        // The book's folder, and its entry.
        QCOMPARE(trash.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), inTrash + 2);
        QCOMPARE(shelf.run({"folio", "remove", shelf.dir.filePath("First Book.epub")}).object().value("error").toArray(),
                 (QJsonArray{"Not in a library."}));

        // A change made on the command line shows in the view at once.
        shelf.run({"set", QString::number(books.at(1).id), "--title", "Retitled"});
        books = shelf.books();
        QCOMPARE(listing(shelf.view()), QStringList{nameOf(books.at(0))});
        QVERIFY(nameOf(books.at(0)).startsWith("Retitled - "));
    }

    void switchedOffThereIsNothingInTheFileManager() {
        Shelf shelf({"First Book"});
        QCOMPARE(shelf.run({"folio", "libraries"}).object().value("items").toArray().size(), 1);
        QCOMPARE(listing(shelf.view()).size(), 1);
        // Switched off in Omabook, the commands Folio runs answer with
        // nothing to show and nothing to offer, and the view goes.
        shelf.settings.setFolio(false);
        const QJsonObject answer = shelf.run({"folio", "libraries"}).object();
        QVERIFY(answer.value("items").toArray().isEmpty());
        QVERIFY(answer.value("actions").isArray() && answer.value("actions").toArray().isEmpty());
        QVERIFY(!QFileInfo::exists(shelf.view()));
        QCOMPARE(shelf.books().size(), 1);
        QVERIFY(shelf.run({"folio", "details", shelf.library + "/" + shelf.books().first().path}).array().isEmpty());
    }

    void thePluginIsWhatFolioExpects() {
        const QDir plugins(QStringLiteral(OMABOOK_SOURCE) + "/share/folio");
        // One plugin: the libraries.
        QCOMPARE(plugins.entryList(QDir::Dirs | QDir::NoDotAndDotDot), QStringList{"omabook"});
        QFile file(plugins.filePath("omabook/manifest.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QJsonParseError error;
        const QJsonObject manifest = QJsonDocument::fromJson(file.readAll(), &error).object();
        QCOMPARE(error.error, QJsonParseError::NoError);
        QCOMPARE(manifest.value("folio").toInt(), 1);
        QCOMPARE(manifest.value("requires").toArray(), QJsonArray{"omabook"});
        const QJsonObject poll = manifest.value("poll").toObject();
        QCOMPARE(poll.value("command").toArray(), (QJsonArray{"omabook", "folio", "libraries"}));
        QVERIFY(poll.value("every").toInt() >= 5 && poll.value("every").toInt() <= 10);
        QVERIFY(QFileInfo::exists(plugins.filePath("omabook/omabook.svg")));

        QStringList texts{manifest.value("section").toObject().value("title").toString()};
        QSet<QString> ids;
        const QSet<QString> keys{"id", "label", "command", "menu", "to", "under", "types", "files", "single", "busy", "detach"};
        for (const QJsonValue &value : manifest.value("actions").toArray()) {
            const QJsonObject action = value.toObject();
            for (const QString &key : action.keys())
                QVERIFY2(keys.contains(key), qPrintable(key));
            QVERIFY(!action.value("id").toString().isEmpty() && !ids.contains(action.value("id").toString()));
            ids.insert(action.value("id").toString());
            QVERIFY(!action.value("label").toString().isEmpty());
            const QJsonArray command = action.value("command").toArray();
            QCOMPARE((QJsonArray{command.at(0), command.at(1)}), (QJsonArray{"omabook", "folio"}));
            QVERIFY(QStringList({"", "main", "share"}).contains(action.value("menu").toString()));
            QVERIFY(QStringList({"", "items"}).contains(action.value("to").toString()));
            for (const QJsonValue &under : action.value("under").toArray())
                QCOMPARE(QDir::homePath() + under.toString().mid(1), Folio::viewsRoot() + "/");
            for (const QJsonValue &type : action.value("types").toArray())
                QVERIFY(type.toString().contains('/'));
            texts << action.value("label").toString();
            if (action.contains("busy"))
                texts << action.value("busy").toString();
        }
        QCOMPARE(ids, (QSet<QString>{"open", "lookup", "read", "reading", "remove", "add"}));
        QCOMPARE(manifest.value("details").toObject().value("command").toArray(), (QJsonArray{"omabook", "folio", "details"}));

        // Every catalog translates what the English one lists, and that
        // has every text the manifest shows.
        QFile english(plugins.filePath("omabook/i18n/en.json"));
        QVERIFY(english.open(QIODevice::ReadOnly));
        const QStringList known = QJsonDocument::fromJson(english.readAll()).object().keys();
        for (const QString &text : texts)
            QVERIFY2(known.contains(text), qPrintable(text));
        for (const QString &code : {QString("nl"), QString("de"), QString("fr"), QString("es-MX")}) {
            QFile catalog(plugins.filePath("omabook/i18n/" + code + ".json"));
            QVERIFY2(catalog.open(QIODevice::ReadOnly), qPrintable(catalog.fileName()));
            const QJsonObject translated = QJsonDocument::fromJson(catalog.readAll()).object();
            QCOMPARE(translated.keys(), known);
            for (const QString &text : known)
                QCOMPARE(translated.value(text).toString().count('%'), text.count('%'));
        }

        // Every type Omabook offers to open is one its desktop entry opens.
        QFile desktop(QStringLiteral(OMABOOK_SOURCE) + "/share/omabook.desktop");
        QVERIFY(desktop.open(QIODevice::ReadOnly));
        const QString entry = QString::fromUtf8(desktop.readAll());
        for (const QString &type : Folio::ebookTypes())
            QVERIFY2(entry.contains(type + ";"), qPrintable(type));
        for (const QJsonValue &type : manifest.value("details").toObject().value("types").toArray())
            QVERIFY2(entry.contains(type.toString() + ";"), qPrintable(type.toString()));
    }
};

QTEST_MAIN(FolioTests)
#include "folio_tests.moc"
