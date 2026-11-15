#include "folio.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QEventLoopLocker>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

#include "formats.h"
#include "importer.h"
#include "library.h"

using namespace Qt::StringLiterals;

namespace {

// A file is taken as copied in once its size and time have stayed the same
// for this long, across two looks: Folio and most copying tools write a
// file under its final name, so a half-copied book looks like any other
// until it stops growing.
constexpr qint64 settleMs = 2000;
// A removed book's details are kept this long, so restoring its entry from
// the trash brings the book back as it was.
constexpr qint64 rememberRemovedSecs = 30 * 24 * 3600;

// A library that is there; one whose folder is gone (an unplugged drive, a
// machine that is off) is left out rather than created by looking at it.
std::unique_ptr<Library> openLibrary(const QString &path) {
    if (!QFileInfo::exists(path + "/omabook.db"))
        return nullptr;
    auto library = std::make_unique<Library>(path);
    return library->open() ? std::move(library) : nullptr;
}

QString cleanPath(const QString &path) { return QDir::cleanPath(QFileInfo(path).absoluteFilePath()); }
QString libraryName(const QString &path) { return QDir(path).dirName(); }

// Text Folio translates from the plugin's catalog: a template and its values.
QJsonArray text(const QString &english, const QStringList &values = {}) {
    return QJsonArray::fromStringList(QStringList{english} + values);
}
QJsonArray row(const QString &label, const QString &value) { return {label, value}; }

void printJson(const QJsonDocument &document) {
    fputs(document.toJson(QJsonDocument::Compact).constData(), stdout);
    fputc('\n', stdout);
}

// Which file a name is, whatever it is called: the device and inode.
struct Identity {
    qint64 device = 0, inode = 0;
    bool operator==(const Identity &) const = default;
    explicit operator bool() const noexcept { return inode != 0; }
    QJsonArray toJson() const { return {device, inode}; }
    static Identity fromJson(const QJsonValue &value) {
        const QJsonArray pair = value.toArray();
        return {pair.at(0).toInteger(), pair.at(1).toInteger()};
    }
};
size_t qHash(const Identity &identity, size_t seed = 0) noexcept { return qHashMulti(seed, identity.device, identity.inode); }

// `follow` looks through a symbolic link at the file it names; without it,
// the name itself, which a rename keeps.
Identity identityOf(const QString &path, bool follow) {
    struct stat info {};
    const QByteArray name = QFile::encodeName(path);
    if ((follow ? ::stat(name.constData(), &info) : ::lstat(name.constData(), &info)) != 0)
        return {};
    return {static_cast<qint64>(info.st_dev), static_cast<qint64>(info.st_ino)};
}

// What is remembered of a view, beside it: which entry is which book, the
// files copied in that are still being looked at, and the books its
// deleted entries took to the trash.
struct ViewState {
    QString library;
    Identity folder;
    qint64 changes = -1;    // the library's change counter at the last pass
    int books = 0, reading = 0;
    struct Entry {
        qint64 id = 0;
        Identity self;      // the entry's own name (a link's, for a link)
    };
    QMap<QString, Entry> entries;
    struct Seen {
        qint64 size = 0, modified = 0, since = 0;
    };
    QMap<QString, Seen> pending;
    QMap<QString, Seen> failed; // book files that couldn't be added, as they were then
    QJsonObject ignored;    // name: why it wasn't added
    QJsonObject removed;    // a removed book's file hash: what it was

    QJsonObject toJson() const {
        QJsonObject entryObjects;
        for (auto it = entries.begin(); it != entries.end(); ++it)
            entryObjects.insert(it.key(), QJsonObject{{"id", it->id}, {"self", it->self.toJson()}});
        return {{"library", library}, {"folder", folder.toJson()}, {"changes", changes}, {"books", books},
                {"reading", reading}, {"entries", entryObjects}, {"pending", seenJson(pending)},
                {"failed", seenJson(failed)}, {"ignored", ignored}, {"removed", removed}};
    }
    static QJsonObject seenJson(const QMap<QString, Seen> &files) {
        QJsonObject object;
        for (auto it = files.begin(); it != files.end(); ++it)
            object.insert(it.key(), QJsonArray{it->size, it->modified, it->since});
        return object;
    }
    static QMap<QString, Seen> seenFrom(const QJsonObject &object) {
        QMap<QString, Seen> files;
        for (auto it = object.begin(); it != object.end(); ++it) {
            const QJsonArray seen = it->toArray();
            files.insert(it.key(), {seen.at(0).toInteger(), seen.at(1).toInteger(), seen.at(2).toInteger()});
        }
        return files;
    }
    static ViewState fromJson(const QJsonObject &object) {
        ViewState state;
        state.library = object.value("library").toString();
        state.folder = Identity::fromJson(object.value("folder"));
        state.changes = object.value("changes").toInteger(-1);
        state.books = object.value("books").toInt();
        state.reading = object.value("reading").toInt();
        const QJsonObject entries = object.value("entries").toObject();
        for (auto it = entries.begin(); it != entries.end(); ++it)
            state.entries.insert(it.key(), {it->toObject().value("id").toInteger(),
                                            Identity::fromJson(it->toObject().value("self"))});
        state.pending = seenFrom(object.value("pending").toObject());
        state.failed = seenFrom(object.value("failed").toObject());
        state.ignored = object.value("ignored").toObject();
        state.removed = object.value("removed").toObject();
        return state;
    }
};

QString statePath(const QString &view) { return QFileInfo(view).absolutePath() + "/." + QFileInfo(view).fileName() + ".json"; }
QString lockPath(const QString &view) { return QFileInfo(view).absolutePath() + "/." + QFileInfo(view).fileName() + ".lock"; }

std::optional<ViewState> loadState(const QString &view) {
    QFile file(statePath(view));
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? std::optional(ViewState::fromJson(document.object())) : std::nullopt;
}

bool saveState(const QString &view, const ViewState &state) {
    QDir().mkpath(QFileInfo(view).absolutePath());
    QSaveFile file(statePath(view));
    file.setDirectWriteFallback(false);
    return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(state.toJson()).toJson(QJsonDocument::Compact)) >= 0
           && file.commit();
}

// A name for the book a folder can hold: no slashes or control characters,
// not hidden, and short enough for any filesystem with its extension.
QString entryName(const Book &book, const QString &extension) {
    const QString title = book.meta.title.trimmed().isEmpty() ? u"Untitled"_s : book.meta.title.trimmed();
    QString name = book.meta.authors.isEmpty() ? title : title + " - " + book.meta.authorText();
    for (QChar &c : name)
        if (c == '/' || c == '\\' || c.category() == QChar::Other_Control)
            c = c == '/' || c == '\\' ? QChar('-') : QChar(' ');
    name = name.simplified();
    while (name.startsWith('.'))
        name.remove(0, 1);
    if (name.isEmpty())
        name = u"Untitled"_s;
    // 255 bytes is the most a name may be on the usual filesystems; leave
    // room for the extension and a number that tells two books apart.
    while (name.toUtf8().size() > 200)
        name.chop(1);
    return name.trimmed() + "." + extension;
}

// Files that are on their way and say so by their name: they are neither
// books nor anything to report until they arrive.
bool inProgress(const QString &name) {
    return name.endsWith(".part"_L1) || name.endsWith(".crdownload"_L1) || name.endsWith(".tmp"_L1) || name.endsWith('~');
}

// Removes a name Omabook made, only while it is still the file Omabook made.
bool removeOwn(const QString &path, const Identity &self) {
    return identityOf(path, false) == self && ::unlink(QFile::encodeName(path).constData()) == 0;
}

// An entry for `file` at `path`: a hard link where the filesystem allows one,
// a symbolic link otherwise. Made under a hidden name and renamed into place,
// so a half-made entry never shows.
Identity makeEntry(const QString &file, const QString &path) {
    const QString temporary = QFileInfo(path).absolutePath() + "/.omabook-" + QString::number(QCoreApplication::applicationPid()) + ".new";
    ::unlink(QFile::encodeName(temporary).constData());
    if (::link(QFile::encodeName(file).constData(), QFile::encodeName(temporary).constData()) != 0
        && !QFile::link(file, temporary)) {
        qWarning().noquote() << "No entry could be made for" << file << "in" << QFileInfo(path).absolutePath();
        return {};
    }
    if (QFileInfo(path).exists() || QFileInfo(path).isSymLink()
        || ::rename(QFile::encodeName(temporary).constData(), QFile::encodeName(path).constData()) != 0) {
        ::unlink(QFile::encodeName(temporary).constData());
        return {};
    }
    return identityOf(path, false);
}

// What is kept of a book that goes to the trash from its view, so that
// bringing the entry back from the trash brings the book back as it was:
// its details, its reading, its bookmarks and highlights.
QJsonObject snapshot(const Library &library, const Book &book, const QString &trashed) {
    const Progress progress = library.progress(book.id);
    QJsonArray annotations;
    for (const Annotation &annotation : library.annotations(book.id))
        annotations.append(QJsonObject{{"kind", annotation.kind}, {"section", annotation.section},
                                       {"fraction", annotation.fraction}, {"text", annotation.text},
                                       {"note", annotation.note}, {"created", annotation.created}});
    return {{"meta", book.meta.toJson()}, {"shelf", book.shelf}, {"rating", book.rating}, {"finished", book.finished},
            {"paper", book.paper}, {"identifiers", book.identifiers},
            {"progress", QJsonArray{progress.section, progress.fraction, progress.percent, progress.updated}},
            {"annotations", annotations}, {"trashed", trashed}, {"cover", trashed.isEmpty() ? QString() : trashed + "/cover.jpg"},
            {"at", QDateTime::currentSecsSinceEpoch()}};
}

void restore(Library &library, qint64 id, const QJsonObject &kept) {
    BookMeta meta = BookMeta::fromJson(kept.value("meta").toObject());
    if (const QString cover = kept.value("cover").toString(); !cover.isEmpty() && QFileInfo::exists(cover))
        meta.cover = QImage(cover);
    library.setMeta(id, meta);
    library.setReading(id, kept.value("shelf").toString(), kept.value("rating").toInt(), kept.value("finished").toString());
    library.setPaper(id, kept.value("paper").toBool());
    const QJsonObject identifiers = kept.value("identifiers").toObject();
    for (auto it = identifiers.begin(); it != identifiers.end(); ++it)
        library.setIdentifier(id, it.key(), it->toString());
    const QJsonArray progress = kept.value("progress").toArray();
    if (progress.at(2).toDouble() > 0)
        library.setProgress(id, {progress.at(0).toInt(), progress.at(1).toDouble(), progress.at(2).toDouble(),
                                 progress.at(3).toInteger()});
    for (const QJsonValue &value : kept.value("annotations").toArray()) {
        const QJsonObject a = value.toObject();
        library.addAnnotation(id, {0, a.value("kind").toString(), a.value("section").toInt(), a.value("fraction").toDouble(),
                                   a.value("text").toString(), a.value("note").toString(), a.value("created").toInteger()});
    }
}

// The trash in the home folder, made when there is none yet, as the trash's
// specification allows anyone to: Qt only moves something to a trash that
// is there already.
void makeHomeTrash() {
    const QString trash = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/Trash";
    QDir().mkpath(QFileInfo(trash).absolutePath());
    for (const QString &folder : {trash, trash + "/files", trash + "/info"})
        if (!QFileInfo::exists(folder))
            QDir().mkdir(folder, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

// Sends a book to the trash, the way removing it in Omabook does, but only
// once its folder is safely in the trash: where there is no trash, the book
// stays, since nobody was asked whether it may go for good.
bool trashBook(Library &library, const Book &book, ViewState &state) {
    const QString folder = library.root() + "/" + book.path;
    QString trashed;
    makeHomeTrash();
    if (QFileInfo::exists(folder) && !QFile::moveToTrash(folder, &trashed)) {
        qWarning().noquote() << "The entry of" << book.meta.title << "was deleted, but its folder can't go to the trash;"
                             << "the book stays in" << library.root();
        return false;
    }
    const QJsonObject kept = snapshot(library, book, trashed);
    const QList<FormatFile> files = library.formatFiles(book.id);
    if (!library.remove(book.id))
        return false;
    for (const FormatFile &file : files)
        state.removed.insert(file.hash, kept);
    return true;
}

void forgetOldRemovals(ViewState &state) {
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (const QString &hash : state.removed.keys())
        if (now - state.removed.value(hash).toObject().value("at").toInteger() > rememberRemovedSecs)
            state.removed.remove(hash);
}

// What the view should hold: an entry per book that has a file.
struct Wanted {
    qint64 id = 0;
    QString file;
    QString name;
    Identity identity;
};

QList<Wanted> wanted(const Library &library, const QList<Book> &books) {
    QList<Wanted> wants;
    QSet<QString> names;
    for (const Book &book : books) {
        const QString file = library.formatPath(book.id);
        const Identity identity = file.isEmpty() ? Identity{} : identityOf(file, true);
        if (!identity)
            continue;
        const QString extension = Formats::extension(file);
        QString name = entryName(book, extension);
        // Two books of the same title and author: the later one says which.
        if (names.contains(name.toLower()))
            name = name.chopped(extension.size() + 1) + QString(" (%1).").arg(book.id) + extension;
        names.insert(name.toLower());
        wants.append({book.id, file, name, identity});
    }
    return wants;
}

// What is in the view folder: its names, without hidden ones.
QFileInfoList listing(const QString &view) {
    return QDir(view).entryInfoList(QDir::Files | QDir::Dirs | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
}

// Names anywhere below the view's top, by identity: an entry moved into a
// folder of its own in the view hasn't left the library.
QSet<Identity> inodesBelow(const QString &view) {
    QSet<Identity> inodes;
    for (const QFileInfo &folder : listing(view)) {
        if (!folder.isDir() || folder.isSymLink())
            continue;
        QDirIterator it(folder.filePath(), QDir::Files | QDir::System | QDir::NoDotAndDotDot | QDir::Hidden,
                        QDirIterator::Subdirectories);
        while (it.hasNext())
            inodes.insert(identityOf(it.next(), false));
    }
    return inodes;
}

bool upToDate(const QString &view, const ViewState &state, const Library &library) {
    if (state.changes != library.changes().books || !state.pending.isEmpty() || state.folder != identityOf(view, false))
        return false;
    QStringList names;
    for (const QFileInfo &info : listing(view))
        if (!inProgress(info.fileName()))
            names << info.fileName();
    QStringList known = state.entries.keys() + state.ignored.keys();
    known.sort();
    names.sort();
    return names == known;
}

// Starts `omabook <arguments>` on its own, in a session of its own: Folio
// ends a command's whole process group once the command is done.
bool startOwn(const QStringList &arguments, bool window) {
    QProcess process;
    process.setProgram(QCoreApplication::applicationFilePath());
    process.setArguments(arguments);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    if (window) {
        // This command itself runs offscreen, as every command does; the
        // window is given back the display the desktop had set, or it would
        // open where nobody can see it.
        const QString desktop = environment.value("OMABOOK_DESKTOP_QPA");
        environment.remove("OMABOOK_DESKTOP_QPA");
        if (desktop.isEmpty() || desktop == "-")
            environment.remove("QT_QPA_PLATFORM");
        else
            environment.insert("QT_QPA_PLATFORM", desktop);
    }
    process.setProcessEnvironment(environment);
    process.setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession);
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    return process.startDetached();
}

// A pass that found a book file ready to add, with imports deferred, hands
// the adding to a process of its own, which may take as long as copying it
// takes.
void syncThenImport(const QString &library) {
    if (Folio::sync(library, Folio::Imports::Defer).importable)
        startOwn({"folio", "sync", library}, false);
}

QStringList knownLibraries(const Settings &settings) {
    QStringList paths;
    for (const QString &path : settings.libraries() << settings.libraryPath())
        if (!paths.contains(cleanPath(path)))
            paths << cleanPath(path);
    return paths;
}

// A book's folder in a library is "Author/Title (id)"; the number names it
// without a search through the library.
std::optional<Folio::Found> inLibraryFolder(const QString &root, const QString &canonical) {
    const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
    if (canonicalRoot.isEmpty() || !canonical.startsWith(canonicalRoot + "/"))
        return std::nullopt;
    const QString folder = QDir(canonicalRoot).relativeFilePath(QFileInfo(canonical).absolutePath());
    const auto library = openLibrary(root);
    if (!library)
        return std::nullopt;
    static const QRegularExpression numbered(R"(\((\d+)\)$)");
    if (const auto match = numbered.match(folder); match.hasMatch()) {
        const qint64 id = match.captured(1).toLongLong();
        if (const auto book = library->book(id); book && book->path == folder)
            return Folio::Found{root, id};
    }
    for (const Book &book : library->books())
        if (book.path == folder)
            return Folio::Found{root, book.id};
    return std::nullopt;
}

std::optional<Folio::Found> inView(const QString &root, const QString &file) {
    const QString view = Folio::viewFolder(root);
    const QString canonicalView = QFileInfo(view).canonicalFilePath();
    if (canonicalView.isEmpty() || QFileInfo(QFileInfo(file).absolutePath()).canonicalFilePath() != canonicalView)
        return std::nullopt;
    const auto state = loadState(view);
    const auto library = state ? openLibrary(root) : nullptr;
    if (!library)
        return std::nullopt;
    const Identity self = identityOf(file, false);
    const Identity target = identityOf(file, true);
    for (auto it = state->entries.begin(); it != state->entries.end(); ++it) {
        if (it.key() != QFileInfo(file).fileName() && it->self != self)
            continue;
        // Still the book's file: an entry the view hasn't caught up with
        // yet is not taken for a book it no longer is.
        const QString path = library->formatPath(it->id);
        if (!path.isEmpty() && identityOf(path, true) == target)
            return Folio::Found{root, it->id};
    }
    return std::nullopt;
}

Folio::Synced syncLocked(const QString &root, const QString &view, Folio::Imports imports) {
    Folio::Synced result;
    Library library(root);
    if (!library.open())
        return result;
    ViewState state = loadState(view).value_or(ViewState{});
    const Identity folder = identityOf(view, false);
    // A view that is new, or isn't the folder that was there (it was deleted
    // and made again, or something else is mounted there), starts afresh:
    // nothing it held before can have been deleted from it.
    if (!folder || folder != state.folder) {
        state.entries.clear();
        state.pending.clear();
        state.changes = -1;
        state.library = root;
        if (!saveState(view, state) || !QDir().mkpath(view))
            return result;
        state.folder = identityOf(view, false);
    }
    if (upToDate(view, state, library))
        return result;

    QList<Book> books = library.books();
    QList<Wanted> wants = wanted(library, books);
    QHash<Identity, qint64> bookOf; // the library's files: which book each is
    QSet<qint64> withFile;
    for (const Wanted &want : wants) {
        bookOf.insert(want.identity, want.id);
        withFile.insert(want.id);
    }

    // What is there now, sorted out.
    QMap<qint64, QPair<QString, Identity>> claimed; // book: its entry's name and identity
    QStringList seenEntries;                       // remembered entries still there, under any name
    QStringList candidates;
    QJsonObject ignored;
    QStringList extras;
    for (const QFileInfo &info : listing(view)) {
        const QString name = info.fileName();
        const Identity self = identityOf(info.filePath(), false);
        const Identity target = identityOf(info.filePath(), true);
        // A remembered entry, by its name or (renamed in Folio) by its file.
        QString remembered = state.entries.contains(name) && state.entries.value(name).self == self ? name : QString();
        for (auto it = state.entries.cbegin(); remembered.isEmpty() && it != state.entries.cend(); ++it)
            if (it->self == self)
                remembered = it.key();
        if (!remembered.isEmpty())
            seenEntries << remembered;
        const qint64 id = bookOf.value(target, 0);
        if (target && id != 0) {
            // One of the library's own files: an entry, or a second name for
            // one (a link copied within the view), which loses nothing by going.
            if (claimed.contains(id))
                extras << info.filePath();
            else
                claimed.insert(id, {name, self});
            continue;
        }
        if (!remembered.isEmpty()) {
            // An entry for a file the library no longer has (it was replaced
            // by another of the same format): made again below.
            removeOwn(info.filePath(), self);
            continue;
        }
        if (info.isSymLink() && !target) {
            // A link that leads nowhere: one of Omabook's after its book went.
            if (info.symLinkTarget().startsWith(root + "/"))
                QFile::remove(info.filePath());
            else
                ignored.insert(name, "a link to nothing");
            continue;
        }
        if (info.isDir()) {
            ignored.insert(name, "a folder; drop the books in it instead");
            continue;
        }
        if (inProgress(name))
            continue;
        if (!Formats::isBook(name)) {
            ignored.insert(name, "not a book");
            continue;
        }
        candidates << info.filePath();
    }
    for (const QString &extra : extras)
        QFile::remove(extra);

    // Entries that were there and went: their books go to the trash. Not
    // when every one went at once, which is a view that was emptied (or
    // replaced) rather than books thrown away one by one.
    QStringList missing;
    for (auto it = state.entries.begin(); it != state.entries.end(); ++it)
        if (!seenEntries.contains(it.key()))
            missing << it.key();
    const bool emptied = missing.size() > 1 && missing.size() == state.entries.size();
    if (emptied)
        qWarning().noquote() << "Every entry of" << view << "went at once; the books stay in" << root;
    bool changed = false;
    if (!missing.isEmpty() && !emptied) {
        const QSet<Identity> below = inodesBelow(view);
        for (const QString &name : missing) {
            const ViewState::Entry entry = state.entries.value(name);
            const auto book = library.book(entry.id);
            // The book must still be here with its file (a library halfway
            // through a sync may lack it), and the entry not merely moved to
            // a folder within the view.
            if (!book || !withFile.contains(entry.id) || claimed.contains(entry.id) || below.contains(entry.self))
                continue;
            if (trashBook(library, *book, state)) {
                ++result.removed;
                changed = true;
            }
        }
    }

    // Book files copied in: added once they have settled.
    QMap<QString, ViewState::Seen> pending, failed;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const QString &path : candidates) {
        const QFileInfo info(path);
        const QString name = info.fileName();
        const ViewState::Seen seen{info.size(), info.lastModified().toMSecsSinceEpoch(), now};
        // One that couldn't be added is tried again only once it changes.
        if (const auto tried = state.failed.constFind(name);
            tried != state.failed.cend() && tried->size == seen.size && tried->modified == seen.modified) {
            failed.insert(name, *tried);
            ignored.insert(name, state.ignored.value(name).toString(u"not a book Omabook can read"_s));
            continue;
        }
        const auto before = state.pending.constFind(name);
        const bool settled = before != state.pending.cend() && before->size == seen.size && before->modified == seen.modified
                             && now - before->since >= settleMs && seen.size > 0;
        if (!settled) {
            pending.insert(name, before != state.pending.cend() && before->size == seen.size && before->modified == seen.modified
                                     ? *before : seen);
            result.pending = true;
            continue;
        }
        if (imports == Folio::Imports::Defer) {
            pending.insert(name, *before);
            result.importable = true;
            continue;
        }
        const ImportSummary summary = importBooks(library, {path});
        const ImportSummary::Item item = summary.items.value(0);
        const QString kept = item.id ? library.formatPath(item.id) : QString();
        if (item.id == 0 || kept.isEmpty() || !QFileInfo::exists(kept)) {
            ignored.insert(name, item.error.isEmpty() ? summary.problems.value(0, u"not a book Omabook can read"_s) : item.error);
            failed.insert(name, seen);
            continue;
        }
        if (!item.duplicate) {
            for (const FormatFile &file : library.formatFiles(item.id))
                if (state.removed.contains(file.hash)) {
                    restore(library, item.id, state.removed.take(file.hash).toObject());
                    break;
                }
        }
        // The library has its own copy now; the file dropped here gives way
        // to the book's entry.
        if (identityOf(kept, true) != identityOf(path, true))
            QFile::remove(path);
        ++result.imported;
        changed = true;
    }
    state.pending = pending;
    state.failed = failed;

    if (changed) {
        books = library.books();
        wants = wanted(library, books);
    }

    // Every book's entry under its own name, pointing at its file.
    QMap<QString, ViewState::Entry> entries;
    QSet<QString> taken;
    for (const Wanted &want : wants) {
        const auto has = claimed.constFind(want.id);
        if (has != claimed.cend() && has->first == want.name && identityOf(view + "/" + has->first, true) == want.identity) {
            entries.insert(want.name, {want.id, has->second});
            taken.insert(want.name);
            continue;
        }
        if (has != claimed.cend())
            removeOwn(view + "/" + has->first, has->second);
        QString name = want.name;
        if (taken.contains(name) || QFileInfo(view + "/" + name).exists() || QFileInfo(view + "/" + name).isSymLink()) {
            const QString extension = Formats::extension(name);
            name = name.chopped(extension.size() + 1) + QString(" (%1).").arg(want.id) + extension;
        }
        const Identity made = makeEntry(want.file, view + "/" + name);
        if (!made)
            continue;
        entries.insert(name, {want.id, made});
        taken.insert(name);
        ++result.linked;
    }
    // Entries of books that went from the library some other way.
    for (auto it = claimed.begin(); it != claimed.end(); ++it)
        if (!entries.contains(it->first))
            removeOwn(view + "/" + it->first, it->second);

    state.entries = entries;
    state.ignored = ignored;
    state.changes = library.changes().books;
    state.books = int(books.size());
    state.reading = int(std::ranges::count_if(books, [](const Book &book) { return book.status() == "reading"; }));
    forgetOldRemovals(state);
    saveState(view, state);
    for (auto it = ignored.begin(); it != ignored.end(); ++it)
        result.ignored << it.key() + ": " + it->toString();
    return result;
}

void removeView(const QString &view) {
    const auto state = loadState(view);
    if (!state)
        return;
    QLockFile lock(lockPath(view));
    if (!lock.tryLock(5000))
        return;
    for (auto it = state->entries.begin(); it != state->entries.end(); ++it)
        removeOwn(view + "/" + it.key(), it->self);
    QFile::remove(statePath(view));
    QDir().rmdir(view);
    lock.unlock();
    QFile::remove(lockPath(view));
}

// The library (and number) of each file; files of no library are left out.
QList<QPair<Folio::Found, QString>> booksOf(const Settings &settings, const QStringList &files) {
    QList<QPair<Folio::Found, QString>> found;
    for (const QString &file : files)
        if (const auto book = Folio::find(settings, file))
            found.append({*book, file});
    return found;
}

QString titleOf(const Library &library, qint64 id) {
    const auto book = library.book(id);
    return book ? book->meta.title : QString();
}

} // namespace

QString Folio::viewsRoot() { return QDir::homePath() + "/.local/state/omabook/folio"; }

QString Folio::viewFolder(const QString &library) {
    const QString root = viewsRoot();
    const QString clean = cleanPath(library);
    // The view the library has had, whatever was made or went since.
    for (const QString &file : QDir(root).entryList({".*.json"}, QDir::Files | QDir::Hidden)) {
        const QString view = root + "/" + file.mid(1).chopped(5);
        if (const auto state = loadState(view); state && state->library == clean)
            return view;
    }
    QString base = QDir(clean).dirName();
    while (base.startsWith('.'))
        base.remove(0, 1);
    if (base.isEmpty())
        base = u"Library"_s;
    // Otherwise the first name free: a folder there that Omabook didn't
    // make is someone else's.
    for (int n = 1;; ++n) {
        const QString view = root + "/" + (n == 1 ? base : QString("%1 %2").arg(base).arg(n));
        if (!QFileInfo::exists(statePath(view)) && !QFileInfo::exists(view))
            return view;
    }
}

Folio::Synced Folio::sync(const QString &library, Imports imports) {
    const QString root = cleanPath(library);
    if (!QFileInfo::exists(root + "/omabook.db"))
        return {};
    const QString view = viewFolder(root);
    QDir().mkpath(viewsRoot());
    // The window, the poll and a command can each find the view out of step
    // at once; only one sets it right.
    QLockFile lock(lockPath(view));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(imports == Imports::Run ? 2000 : 0)) {
        Synced busy;
        busy.busy = true;
        return busy;
    }
    return syncLocked(root, view, imports);
}

void Folio::removeViews(const QStringList &keep) {
    QStringList kept;
    for (const QString &path : keep)
        kept << cleanPath(path);
    const QDir root(viewsRoot());
    for (const QString &file : root.entryList({".*.json"}, QDir::Files | QDir::Hidden)) {
        const QString view = root.filePath(file.mid(1).chopped(5));
        const auto state = loadState(view);
        if (state && !kept.contains(state->library))
            removeView(view);
    }
}

void Folio::follow(const Settings &settings) {
    if (!settings.folio())
        return;
    for (const QString &library : knownLibraries(settings))
        syncThenImport(library);
}

std::optional<Folio::Found> Folio::find(const Settings &settings, const QString &file) {
    const QFileInfo info(file);
    if (!info.isFile())
        return std::nullopt;
    const QString canonical = info.canonicalFilePath();
    const QStringList libraries = knownLibraries(settings);
    for (const QString &root : libraries)
        if (auto found = inLibraryFolder(root, canonical))
            return found;
    for (const QString &root : libraries)
        if (auto found = inView(root, info.absoluteFilePath()))
            return found;
    return std::nullopt;
}

QJsonObject Folio::libraries(const Settings &settings) {
    const QStringList known = knownLibraries(settings);
    removeViews(known);
    QJsonArray items;
    for (const QString &root : known) {
        if (!QFileInfo::exists(root + "/omabook.db"))
            continue;
        syncThenImport(root);
        const QString view = viewFolder(root);
        const ViewState state = loadState(view).value_or(ViewState{});
        QJsonArray rows{row("Folder", root), row("Books", QString::number(state.books))};
        if (state.reading > 0)
            rows.append(row("Being read", QString::number(state.reading)));
        for (auto it = state.ignored.begin(); it != state.ignored.end(); ++it)
            rows.append(row("Not added", it.key() + ": " + it->toString()));
        const QString books = QString::number(state.books);
        items.append(QJsonObject{
            {"id", root},
            {"name", libraryName(root)},
            {"icon", "omabook.svg"},
            {"subtitle", state.ignored.isEmpty() ? text("%1 books", {books})
                                                 : text("%1 books, %2 not added", {books, QString::number(state.ignored.size())})},
            {"details", rows},
            {"open", QJsonObject{{"path", view}}},
        });
    }
    return {{"items", items}};
}

QJsonArray Folio::details(const Settings &settings, const QString &file) {
    const QFileInfo info(file);
    if (!info.isFile())
        return {};
    // A book file in a view that wasn't added says why.
    for (const QString &root : knownLibraries(settings)) {
        const QString view = viewFolder(root);
        if (QFileInfo(info.absolutePath()).canonicalFilePath() != QFileInfo(view).canonicalFilePath())
            continue;
        if (const auto state = loadState(view); state && state->ignored.contains(info.fileName()))
            return {row("Not added", state->ignored.value(info.fileName()).toString())};
        if (const auto state = loadState(view); state && state->pending.contains(info.fileName()))
            return {row("Being added", "…")};
    }
    std::unique_ptr<Library> library;
    std::optional<Book> book;
    QString root;
    if (const auto found = find(settings, file)) {
        library = openLibrary(found->library);
        book = library ? library->book(found->id) : std::nullopt;
        root = found->library;
    } else {
        // Folio waits two seconds at most, and telling whether a library has
        // a file elsewhere means reading all of it; a very large one is not
        // worth the wait.
        if (!Formats::isBook(file) || info.size() > 300 * 1024 * 1024)
            return {};
        const QString hash = Library::hashFile(file);
        for (const QString &path : settings.libraries()) {
            library = openLibrary(path);
            const qint64 id = library ? library->bookWithHash(hash) : 0;
            book = id ? library->book(id) : std::nullopt;
            if (book) {
                root = path;
                break;
            }
        }
    }
    if (!book)
        return {};
    QJsonArray rows{row("Title", book->meta.title)};
    if (!book->meta.authors.isEmpty())
        rows.append(row("Author", book->meta.authorText()));
    if (!book->meta.series.isEmpty())
        rows.append(row("Series", book->meta.seriesIndex > 0
                                      ? QString("%1 #%2").arg(book->meta.series, QLocale().toString(book->meta.seriesIndex))
                                      : book->meta.series));
    const QString status = book->status();
    if (status == "read")
        rows.append(row("Read", book->finished.isEmpty() ? u"✓"_s : book->finished));
    else if (book->percent > 0)
        rows.append(row("Read", QString::number(qRound(book->percent)) + "%"));
    else if (status == "to-read")
        rows.append(row("To read", u"✓"_s));
    if (book->rating > 0)
        rows.append(row("Rating", QString("★").repeated(book->rating)));
    QStringList formats;
    for (const QString &format : book->formats)
        formats << format.toUpper();
    if (!formats.isEmpty())
        rows.append(row("Formats", formats.join(", ")));
    rows.append(row("Omabook", libraryName(root)));
    return rows;
}

QJsonObject Folio::add(const Settings &settings, const QString &library, const QStringList &paths) {
    const QString path = library.isEmpty() ? settings.libraryPath() : library;
    Library target(path);
    QString error;
    if (!target.open(&error))
        return {{"error", error}};
    const ImportSummary summary = importBooks(target, paths);
    if (summary.added == 0 && summary.duplicates == 0)
        return {{"error", summary.problems.isEmpty() ? QJsonValue(text("No books were found.")) : QJsonValue(summary.problems.first())}};
    const QString name = libraryName(path);
    if (summary.added == 0)
        return {{"notice", text("Already in %1.", {name})}};
    if (settings.folio())
        sync(path, Imports::Defer);
    return {{"notice", summary.duplicates > 0
                           ? text("Added %1 to %2; %3 were already there.",
                                  {QString::number(summary.added), name, QString::number(summary.duplicates)})
                           : text("Added %1 to %2.", {QString::number(summary.added), name})}};
}

QJsonObject Folio::mark(const Settings &settings, const QString &status, const QStringList &files) {
    if (status != "read" && status != "reading")
        return {{"error", text("Mark as read or reading.")}};
    const auto books = booksOf(settings, files);
    if (books.isEmpty())
        return {{"error", text("Not in a library.")}};
    for (const auto &[found, file] : books) {
        const auto library = openLibrary(found.library);
        const auto book = library ? library->book(found.id) : std::nullopt;
        if (!book)
            continue;
        const QString finished = status == "read" ? (book->finished.isEmpty() ? QDate::currentDate().toString(Qt::ISODate) : book->finished)
                                                  : QString();
        library->setReading(found.id, status, book->rating, finished);
    }
    if (books.size() == 1) {
        const auto library = openLibrary(books.first().first.library);
        const QString title = library ? titleOf(*library, books.first().first.id) : QString();
        return {{"notice", text(status == "read" ? "Marked %1 as read." : "Marked %1 as being read.", {title})}};
    }
    return {{"notice", text(status == "read" ? "Marked %1 books as read." : "Marked %1 books as being read.",
                            {QString::number(books.size())})}};
}

QJsonObject Folio::remove(const Settings &settings, const QStringList &files) {
    const auto books = booksOf(settings, files);
    if (books.isEmpty())
        return {{"error", text("Not in a library.")}};
    int removed = 0;
    QString title;
    for (const auto &[found, file] : books) {
        const QString view = viewFolder(found.library);
        QLockFile lock(lockPath(view));
        if (!lock.tryLock(10000))
            continue;
        const auto library = openLibrary(found.library);
        const auto book = library ? library->book(found.id) : std::nullopt;
        if (!book)
            continue;
        ViewState state = loadState(view).value_or(ViewState{});
        if (!trashBook(*library, *book, state))
            return {{"error", text("%1 can't go to the trash, so it stays in the library.", {book->meta.title})}};
        // The entry goes to the trash with it: restored from there, it
        // brings the book back.
        const bool entry = QFileInfo(file).absolutePath() == QFileInfo(view).absoluteFilePath();
        if (entry) {
            state.entries.remove(QFileInfo(file).fileName());
            QFile::moveToTrash(file);
        }
        state.changes = -1;
        saveState(view, state);
        ++removed;
        title = book->meta.title;
    }
    if (removed == 1)
        return {{"notice", text("Moved %1 to the trash.", {title})}};
    return {{"notice", text("Moved %1 books to the trash.", {QString::number(removed)})}};
}

QStringList Folio::ebookTypes() {
    return {"application/epub+zip", "application/x-fictionbook+xml", "application/x-zip-compressed-fb2",
            "application/vnd.comicbook+zip", "application/vnd.comicbook-rar", "application/x-cb7", "application/x-cbt",
            "application/x-mobipocket-ebook", "application/x-mobi8-ebook", "application/vnd.amazon.mobi8-ebook",
            "application/vnd.amazon.ebook", "application/x-sony-bbeb", "image/vnd.djvu", "image/vnd.djvu+multipage",
            "application/x-markdown-book"};
}

int Folio::run(const QStringList &arguments) {
    const QString command = arguments.value(2);
    const QStringList rest = arguments.mid(3);
    const Settings settings;
    // Switched off in Omabook's settings, the plugin shows nothing in Folio:
    // no section rows, no details, no menu entries, and no views.
    if (!settings.folio() && command == "libraries") {
        removeViews();
        printJson(QJsonDocument(QJsonObject{{"items", QJsonArray()}, {"actions", QJsonArray()}}));
        return 0;
    }
    if (!settings.folio() && command == "details") {
        printJson(QJsonDocument(QJsonArray()));
        return 0;
    }
    const auto answer = [](const QJsonObject &result) {
        printJson(QJsonDocument(result));
        return result.contains("error") ? 1 : 0;
    };
    const auto window = [&answer](const QStringList &words) {
        return answer(startOwn(words, true) ? QJsonObject() : QJsonObject{{"error", text("Omabook couldn't be started.")}});
    };
    if (command == "libraries") {
        printJson(QJsonDocument(libraries(settings)));
    } else if (command == "details") {
        printJson(QJsonDocument(details(settings, rest.value(0))));
    } else if (command == "add") {
        // Folio names the library chosen in its submenu in the environment.
        return answer(add(settings, qEnvironmentVariable("FOLIO_TARGET"), rest));
    } else if (command == "open" && !rest.isEmpty()) {
        // A book in a library opens in that library, where it was left; any
        // other file opens as Omabook opens any file.
        bool number = false;
        rest.first().toLongLong(&number);
        if (number)
            return window({"open", rest.first()});
        if (const auto found = find(settings, rest.first()))
            return window({"open", "--library", found->library, QString::number(found->id)});
        return window({"open", QFileInfo(rest.first()).absoluteFilePath()});
    } else if (command == "lookup" && !rest.isEmpty()) {
        const auto found = find(settings, rest.first());
        if (!found)
            return answer({{"error", text("Not in a library.")}});
        return window({"open", "--library", found->library, "--look-up", QString::number(found->id)});
    } else if (command == "mark" && !rest.isEmpty()) {
        return answer(mark(settings, rest.first(), rest.mid(1)));
    } else if (command == "remove" && !rest.isEmpty()) {
        return answer(remove(settings, rest));
    } else if (command == "sync" && !rest.isEmpty()) {
        // Started by the poll, or by hand: adds what was copied into the
        // view, waiting for files still being copied, for as long as they
        // keep growing (up to an hour).
        if (!settings.folio())
            return 0;
        const QDateTime giveUp = QDateTime::currentDateTime().addSecs(3600);
        for (Synced synced = sync(rest.first()); synced.pending && QDateTime::currentDateTime() < giveUp;
             synced = sync(rest.first()))
            QThread::msleep(settleMs);
    } else {
        fputs("omabook folio is run by Omabook's plugin for the Folio file manager: libraries, details <file>, "
              "add <files>, open <file|id>, lookup <file>, mark read|reading <files>, remove <files>, sync <library>.\n",
              stderr);
        return 1;
    }
    return 0;
}

Folio::Follower::Follower(Settings *settings, QObject *parent) : QObject(parent), m_settings(settings), m_on(settings->folio()) {
    m_soon.setSingleShot(true);
    m_soon.setInterval(1000);
    connect(&m_soon, &QTimer::timeout, this, &Follower::start);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_soon.start(); });
    connect(m_settings, &Settings::changed, this, &Follower::settingsChanged);
    connect(&m_running, &QFutureWatcher<Synced>::finished, this, [this] {
        const Synced synced = m_running.result();
        watch();
        if (synced.pending || std::exchange(m_again, false))
            m_soon.start(synced.pending ? int(settleMs) + 500 : 0);
    });
}

Folio::Follower::~Follower() { m_running.waitForFinished(); }

void Folio::Follower::setActive(bool active) {
    m_active = active;
    m_on = m_settings->folio();
    libraryChanged();
}

void Folio::Follower::follow(const QString &library) {
    m_library = library;
    libraryChanged();
}

void Folio::Follower::libraryChanged() {
    if (m_active && m_on && !m_library.isEmpty())
        m_soon.start();
}

void Folio::Follower::start() {
    if (!m_active || !m_on || m_library.isEmpty())
        return;
    if (m_running.isRunning()) {
        m_again = true;
        return;
    }
    // Copying a book in can take a while; the window may close meanwhile,
    // and the copy still finishes.
    auto keepRunning = std::make_shared<QEventLoopLocker>();
    m_running.setFuture(QtConcurrent::run([library = m_library, keepRunning] { return sync(library, Imports::Run); }));
}

void Folio::Follower::watch() {
    if (!m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.directories());
    const QString view = m_active && m_on && !m_library.isEmpty() ? viewFolder(m_library) : QString();
    if (!view.isEmpty() && QFileInfo(view).isDir())
        m_watcher.addPath(view);
}

void Folio::Follower::settingsChanged() {
    if (!m_active || m_settings->folio() == m_on)
        return;
    m_on = m_settings->folio();
    m_running.waitForFinished();
    if (m_on) {
        m_soon.start(0);
    } else {
        removeViews();
        watch();
    }
}

void Folio::Follower::checkOpensEbooks() {
    if (!m_active)
        return;
    auto *query = new QProcess(this);
    connect(query, &QProcess::finished, this, [this, query] {
        const bool opens = QString::fromUtf8(query->readAllStandardOutput()).trimmed() == "omabook.desktop";
        query->deleteLater();
        if (opens != m_opensEbooks) {
            m_opensEbooks = opens;
            emit opensEbooksChanged();
        }
    });
    connect(query, &QProcess::errorOccurred, query, &QObject::deleteLater);
    query->start("xdg-mime", {"query", "default", "application/epub+zip"});
}

void Folio::Follower::openEbooksWithOmabook() {
    if (!m_active)
        return;
    auto *set = new QProcess(this);
    connect(set, &QProcess::finished, this, [this, set] {
        set->deleteLater();
        checkOpensEbooks();
    });
    connect(set, &QProcess::errorOccurred, set, &QObject::deleteLater);
    set->start("xdg-mime", QStringList{"default", "omabook.desktop"} + ebookTypes());
}
