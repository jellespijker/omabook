#pragma once

#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <optional>

#include "settings.h"

// Omabook's side of its plugin for Folio, Omarchy's file manager. Folio runs
// commands and reads the JSON they print (see its plugins.md); these are
// those commands' answers, so the manifest in share/folio only names
// `omabook folio …` and holds no logic of its own.
//
// Each library shows in Folio's sidebar as a folder: its view, one entry per
// book named "Title - Author.epub". The views live in this computer's state
// folder, beside nothing of the library's own, so a library that is synced
// or backed up carries no second copy of its list, and another computer's
// half-finished sync can never look like books deleted here. An entry is a
// hard link to the book's file in the library, so it opens, previews and
// copies out as the file itself and follows the library's own folders when
// they are renamed; where a hard link can't be made (the library is on
// another drive) it is a symbolic link instead.
//
// The view is kept in step with the library, and the library with the view:
// a book file copied into a view is added to its library once it has stopped
// changing, and an entry deleted from a view (or moved out of it) sends the
// book to the trash. What was there before is remembered beside the views,
// so only an entry that existed and went counts as deleted, and a view that
// vanished or was emptied all at once removes nothing.
namespace Folio {

// Where the views are: ~/.local/state/omabook/folio. Fixed under the home
// folder rather than following XDG_STATE_HOME, because the plugin's manifest
// names this folder for the actions it offers on entries, and a manifest
// can't follow a variable.
QString viewsRoot();
// The view of a library: the folder its entries are in. Two libraries whose
// folders have the same name get "Name" and "Name 2", for as long as both
// are kept.
QString viewFolder(const QString &library);

// What a pass over a view did.
struct Synced {
    int imported = 0;       // book files copied in and added
    int removed = 0;        // books whose entries were deleted, now in the trash
    int linked = 0;         // entries made or put right
    bool busy = false;      // another pass held the view; nothing was done
    bool pending = false;   // a copied-in file is still changing; look again soon
    bool importable = false; // with imports deferred: a file is ready to be added
    QStringList ignored;    // what is in the view but isn't a book, with why
};

enum class Imports { Run, Defer };

// Brings a library's view and the library in step: entries for every book
// with a file, books for book files copied in (with Imports::Run; the poll,
// which has ten seconds, defers them to a process of their own), and the
// trash for books whose entries were deleted. Cheap when nothing changed on
// either side: a look at the library's change counter and a listing of the
// folder. A library that isn't there (an unplugged drive) is left alone,
// and so is its view.
Synced sync(const QString &library, Imports imports = Imports::Run);
// Every view Omabook made, taken away again: the entries it made and what
// it remembered of them. Files someone else put there stay. With `keep`,
// the views of those libraries stay.
void removeViews(const QStringList &keep = {});
// What `omabook` commands that may have changed a library do on their way
// out: the views of the libraries that changed follow them.
void follow(const Settings &settings);

// A book a file is, found by where it is rather than by its content: a file
// in a library's own folders, a link to one, or an entry in a view.
struct Found {
    QString library;
    qint64 id = 0;
};
std::optional<Found> find(const Settings &settings, const QString &file);

// The poll: each library as an item that opens its view in Folio, with how
// many books it holds.
QJsonObject libraries(const Settings &settings);
// The rows Folio's details pane shows for a book file: for an entry, the
// book's details and how far it was read; for another file, which library
// has it. Empty for a file no library has.
QJsonArray details(const Settings &settings, const QString &file);
// Adds files and folders to a library (the current one when `library` is
// empty); the result is what Folio shows: a notice, or an error.
QJsonObject add(const Settings &settings, const QString &library, const QStringList &paths);
// Marks the books of entries as read or being read.
QJsonObject mark(const Settings &settings, const QString &status, const QStringList &files);
// Sends the books of entries to the trash, and their entries with them, so
// restoring an entry from the trash brings its book back.
QJsonObject remove(const Settings &settings, const QStringList &files);

// The ebook types Omabook offers to open for everyone who asks it to: not
// PDF or Markdown, which other apps are as likely to be wanted for.
QStringList ebookTypes();

// `omabook folio <libraries|details|add|open|lookup|mark|remove|sync|open-library> …`
int run(const QStringList &arguments);

// The window's part: the current library's view follows the library as the
// window sees it change, and the view is watched, so a book copied into it is
// added within seconds rather than at Folio's next poll. The work is done on
// a worker thread. It also says whether Omabook opens ebooks by default.
class Follower : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool opensEbooks READ opensEbooks NOTIFY opensEbooksChanged)

public:
    explicit Follower(Settings *settings, QObject *parent = nullptr);
    ~Follower() override;

    // Nothing is done until the app says so: a window made by a test, with
    // a library of its own, leaves the views in the home folder alone.
    void setActive(bool active);
    // The library the window shows.
    void follow(const QString &library);
    // The library changed: its view follows, soon.
    void libraryChanged();

    bool opensEbooks() const noexcept { return m_opensEbooks; }
    // Asks the desktop whether Omabook opens EPUB files.
    Q_INVOKABLE void checkOpensEbooks();
    // Makes Omabook the app that opens ebooks (ebookTypes), as the person
    // asked in the settings.
    Q_INVOKABLE void openEbooksWithOmabook();

signals:
    void opensEbooksChanged();

private:
    void start();
    void watch();
    void settingsChanged();

    Settings *m_settings;
    QString m_library;
    bool m_on = true;
    bool m_active = false;
    bool m_again = false;
    bool m_opensEbooks = false;
    QFileSystemWatcher m_watcher;
    QTimer m_soon;
    QFutureWatcher<Synced> m_running;
};

} // namespace Folio
