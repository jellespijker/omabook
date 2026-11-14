#include "folio.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <algorithm>
#include <cstdio>

#include "formats.h"
#include "importer.h"
#include "library.h"

namespace {

// A library that is there; one whose folder is gone (an unplugged drive, a
// machine that is off) is left out rather than created by looking at it.
std::unique_ptr<Library> openLibrary(const QString &path) {
    if (!QFileInfo::exists(path + "/omabook.db"))
        return nullptr;
    auto library = std::make_unique<Library>(path);
    return library->open() ? std::move(library) : nullptr;
}

QString libraryName(const QString &path) { return QDir(path).dirName(); }

// Text Folio translates from the plugin's catalog: a template and its values.
QJsonArray text(const QString &english, const QStringList &values) {
    return QJsonArray::fromStringList(QStringList{english} + values);
}
QJsonArray row(const QString &label, const QString &value) { return {label, value}; }

void printJson(const QJsonDocument &document) {
    fputs(document.toJson(QJsonDocument::Compact).constData(), stdout);
    fputc('\n', stdout);
}

} // namespace

QJsonObject Folio::libraries(const Settings &settings) {
    QJsonArray items;
    for (const QString &path : settings.libraries()) {
        const auto library = openLibrary(path);
        if (!library)
            continue;
        const QList<Book> books = library->books();
        const auto reading = std::ranges::count_if(books, [](const Book &book) { return book.status() == "reading"; });
        QJsonArray rows{row("Folder", path), row("Books", QString::number(books.size()))};
        if (reading > 0)
            rows.append(row("Being read", QString::number(reading)));
        items.append(QJsonObject{
            {"id", path},
            {"name", libraryName(path)},
            {"icon", "omabook.svg"},
            {"subtitle", text("%1 books", {QString::number(books.size())})},
            {"details", rows},
            {"open", QJsonObject{{"run", QJsonArray{"omabook", "folio", "open-library", path}}}},
        });
    }
    return {{"items", items}};
}

QJsonObject Folio::reading(const Settings &settings) {
    QJsonArray items;
    if (const auto library = openLibrary(settings.libraryPath())) {
        QList<Book> books = library->books("status:reading");
        std::ranges::sort(books, std::greater{}, &Book::lastRead);
        for (const Book &book : books.mid(0, 30)) {
            QJsonArray rows;
            if (!book.meta.authors.isEmpty())
                rows.append(row("Author", book.meta.authorText()));
            if (!book.meta.series.isEmpty())
                rows.append(row("Series", book.meta.series));
            if (book.percent > 0)
                rows.append(row("Read", QString::number(qRound(book.percent)) + "%"));
            items.append(QJsonObject{
                {"id", QString::number(book.id)},
                {"name", book.meta.title},
                {"icon", "omabook.svg"},
                {"subtitle", book.meta.authorText()},
                {"details", rows},
                {"open", QJsonObject{{"run", QJsonArray{"omabook", "folio", "open", QString::number(book.id)}}}},
            });
        }
    }
    return {{"items", items}};
}

QJsonArray Folio::details(const Settings &settings, const QString &file) {
    // Folio waits two seconds at most, and telling whether a library has a
    // file means reading all of it; a very large one is not worth the wait.
    const QFileInfo info(file);
    if (!info.isFile() || !Formats::isBook(file) || info.size() > 300 * 1024 * 1024)
        return {};
    const QString hash = Library::hashFile(file);
    for (const QString &path : settings.libraries()) {
        const auto library = openLibrary(path);
        const qint64 id = library ? library->bookWithHash(hash) : 0;
        const auto book = id ? library->book(id) : std::nullopt;
        if (!book)
            continue;
        QJsonArray rows{row("Omabook", libraryName(path))};
        if (book->status() == "read")
            rows.append(row("Read", book->finished.isEmpty() ? "✓" : book->finished));
        else if (book->percent > 0)
            rows.append(row("Read", QString::number(qRound(book->percent)) + "%"));
        if (book->rating > 0)
            rows.append(row("Rating", QString("★").repeated(book->rating)));
        return rows;
    }
    return {};
}

QJsonObject Folio::add(const Settings &settings, const QString &library, const QStringList &paths) {
    const QString path = library.isEmpty() ? settings.libraryPath() : library;
    Library target(path);
    QString error;
    if (!target.open(&error))
        return {{"error", error}};
    const ImportSummary summary = importBooks(target, paths);
    if (summary.added == 0 && summary.duplicates == 0)
        return {{"error", summary.problems.isEmpty() ? QJsonValue(text("No books were found.", {})) : QJsonValue(summary.problems.first())}};
    const QString name = libraryName(path);
    if (summary.added == 0)
        return {{"notice", text("Already in %1.", {name})}};
    return {{"notice", summary.duplicates > 0
                           ? text("Added %1 to %2; %3 were already there.",
                                  {QString::number(summary.added), name, QString::number(summary.duplicates)})
                           : text("Added %1 to %2.", {QString::number(summary.added), name})}};
}

int Folio::run(const QStringList &arguments) {
    const QString command = arguments.value(2);
    const QStringList rest = arguments.mid(3);
    const Settings settings;
    // Switched off in Omabook's settings, the plugins show nothing in Folio:
    // no section rows, no details, and no "Add to Library" entry.
    if (!settings.folio() && (command == "libraries" || command == "reading")) {
        printJson(QJsonDocument(QJsonObject{{"items", QJsonArray()}, {"actions", QJsonArray()}}));
        return 0;
    }
    if (!settings.folio() && command == "details") {
        printJson(QJsonDocument(QJsonArray()));
        return 0;
    }
    if (command == "libraries") {
        printJson(QJsonDocument(libraries(settings)));
    } else if (command == "reading") {
        printJson(QJsonDocument(reading(settings)));
    } else if (command == "details") {
        printJson(QJsonDocument(details(settings, rest.value(0))));
    } else if (command == "add") {
        // Folio names the library chosen in its submenu in the environment.
        const QJsonObject result = add(settings, qEnvironmentVariable("FOLIO_TARGET"), rest);
        printJson(QJsonDocument(result));
        return result.contains("error") ? 1 : 0;
    } else if (command == "open" || command == "open-library") {
        // Folio ends a command's whole process group when the command is
        // done, so the window is started detached, in a session of its own.
        // This command itself runs offscreen, as every command does; the
        // window is given back the display the desktop had set, or it would
        // open where nobody can see it.
        QProcess window;
        window.setProgram(QCoreApplication::applicationFilePath());
        window.setArguments(command == "open" ? QStringList{"open", rest.value(0)} : QStringList{"open", "--library", rest.value(0)});
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        const QString desktop = environment.value("OMABOOK_DESKTOP_QPA");
        environment.remove("OMABOOK_DESKTOP_QPA");
        if (desktop.isEmpty() || desktop == "-")
            environment.remove("QT_QPA_PLATFORM");
        else
            environment.insert("QT_QPA_PLATFORM", desktop);
        window.setProcessEnvironment(environment);
        window.setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession);
        window.setStandardOutputFile(QProcess::nullDevice());
        window.setStandardErrorFile(QProcess::nullDevice());
        if (rest.isEmpty() || !window.startDetached()) {
            printJson(QJsonDocument(QJsonObject{{"error", "Omabook couldn't be started."}}));
            return 1;
        }
        printJson(QJsonDocument(QJsonObject()));
    } else {
        fputs("omabook folio is run by Omabook's plugins for the Folio file manager: libraries, reading, details <file>, "
              "add <files>, open <id>, open-library <folder>.\n", stderr);
        return 1;
    }
    return 0;
}
