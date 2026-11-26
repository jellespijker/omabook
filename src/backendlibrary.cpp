// The window's library: which is open, switching to another, and what it does
// when the one asked for isn't there, or goes away while open (#221): it says
// so and makes none in its place unless asked.

#include "backend.h"

#include <QFileInfo>

#include "libraries.h"
#include "librarypresence.h"
#include "scriptplugins.h"

void Backend::openLibrary(const QString &path, bool create) {
    auto library = std::make_unique<Library>(path);
    QString error;
    if (library->createIfMissing(create).open(&error)) {
        showLibrary(std::move(library), {}, false);
        return;
    }
    const bool missing = !create && LibraryPresence::missing(path);
    showLibrary(nullptr, missing ? notThere(path) : error, missing);
}

void Backend::showLibrary(std::unique_ptr<Library> library, const QString &error, bool missing) {
    m_library = std::move(library);
    m_libraryError = error;
    m_libraryMissing = missing;
    emit libraryChanged();
    m_fileManager.follow(m_library ? m_library->root() : QString());
    // What was known of the other library's books is no use here.
    m_model.setKnowledge({});
    takeShelf();
    reload();
}

QString Backend::notThere(const QString &path) {
    return tr("%1 can't be found. Is its drive connected, or its share mounted? Nothing was made in its place.").arg(path);
}

// Gone while the window was open: closed, so nothing more is written, by way
// of the files still open or at the path it was at.
void Backend::lookForLibrary() {
    if (!m_library || LibraryPresence::there(m_library->root()))
        return;
    const QString root = m_library->root();
    showLibrary(nullptr, notThere(root), true);
    say(m_libraryError);
}

void Backend::retryLibrary() { switchTo(m_settings->libraryPath(), false); }

void Backend::createLibrary() { switchTo(m_settings->libraryPath(), true); }

// Opens the library at `path` in place of the one shown, and tells the plugins.
void Backend::switchTo(const QString &path, bool create) {
    m_model.setQuery({});
    openLibrary(path, create);
    m_scripts->emitEvent("library.switched", {{"path", libraryPath()}, {"name", libraryName()}});
}

// The database is replaced or rewritten through its journal, which drops a
// file from the watch, so the watch is set again after every change.
void Backend::watchLibrary() {
    const QStringList watched = m_watcher.files() + m_watcher.directories();
    if (!watched.isEmpty())
        m_watcher.removePaths(watched);
    if (!m_library)
        return;
    for (const QString &path : {m_library->root(), m_library->databasePath(), m_library->databasePath() + "-wal"})
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);
}

void Backend::forgetLibrary(const QString &path, bool trash) {
    const QString before = libraryPath();
    QString error;
    if (!(trash ? Libraries::trash(*m_settings, path, &error) : Libraries::forget(*m_settings, path, &error))) {
        say(error);
        return;
    }
    // Forgetting the library in use moved on to another, which is opened.
    if (libraryPath() != before)
        switchTo(m_settings->libraryPath(), false);
}

void Backend::useLibrary(const QString &path) {
    if (path.isEmpty() || QDir(path).absolutePath() == libraryPath())
        return;
    // A Calibre library is Calibre's to keep; its books are brought into the
    // library in use rather than Omabook moving in beside Calibre.
    if (m_library && Libraries::isCalibreOnly(path) && m_plugins.calibre().enabled()) {
        say(tr("%1 is a Calibre library, so its books are being imported into %2 rather than opened there.")
                .arg(QFileInfo(path).fileName(), libraryName()));
        importCalibreLibrary(path);
        return;
    }
    // A folder chosen for the first time may become a library; one the list
    // names already is opened only where it is (#221).
    const bool create = !LibraryPresence::configured(path);
    m_settings->setLibraryPath(path);
    switchTo(path, create);
    if (!m_libraryError.isEmpty())
        say(m_libraryError);
}
