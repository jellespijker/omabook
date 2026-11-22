#include "snapshotrestorecommands.h"

#include <QCommandLineParser>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
#include <print>
#include <tuple>

#include "library.h"
#include "librarydiff.h"
#include "librarymigrations.h"
#include "settings.h"
#include "snapshotrestore.h"
#include "snapshots.h"
#include "spaces.h"

namespace {

using namespace Qt::StringLiterals;

void printLine(FILE *stream, const QString &text) {
    std::println(stream, "{}", qPrintable(text));
    std::ignore = fflush(stream); // Nothing better to do when the terminal is gone.
}

int fail(const Failure &failure) {
    printLine(stderr, failure.text());
    return 1;
}

void printJson(const QJsonObject &object) {
    printLine(stdout, QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Indented)).trimmed());
}

// A detail as a line shows it: a long description cut short, an empty one "(none)".
[[nodiscard]] QString shown(const QString &value) {
    if (value.isEmpty())
        return u"(none)"_s;
    const QString line = value.simplified();
    return line.size() > 70 ? line.left(69) + u'…' : line;
}

[[nodiscard]] QString named(const Restic::Snapshot &snapshot) {
    return u"snapshot %1 (%2, %3)"_s.arg(snapshot.shortId(), snapshot.time.toString(u"yyyy-MM-dd HH:mm"_s),
                                         snapshot.tags.contains(u"nightly"_s) ? u"nightly"_s : snapshot.tagged(u"reason:"_s));
}

[[nodiscard]] QString bookLine(const LibraryDiff::BookChange &book) {
    return book.authors.isEmpty() ? book.title : u"%1, by %2"_s.arg(book.title, book.authors.join(u" & "_s));
}

// "highlight 2 added, 1 removed; ink 1 changed": the counts there are.
[[nodiscard]] QString countsLine(const QMap<QString, LibraryDiff::Counts> &counts) {
    QStringList kinds;
    for (const auto &[kind, count] : counts.asKeyValueRange()) {
        QStringList said;
        for (const auto &[number, word] :
             {std::pair{count.added, u"added"_s}, {count.removed, u"removed"_s}, {count.changed, u"changed"_s}})
            if (number > 0)
                said << u"%1 %2"_s.arg(number).arg(word);
        if (!said.isEmpty())
            kinds << kind + u' ' + said.join(u", "_s);
    }
    return kinds.join(u"; "_s);
}

void printBooks(const QString &heading, QChar mark, const QList<LibraryDiff::BookChange> &books) {
    if (books.isEmpty())
        return;
    printLine(stdout, u"%1 (%2):"_s.arg(heading).arg(books.size()));
    for (const LibraryDiff::BookChange &book : books) {
        printLine(stdout, u"  %1 %2"_s.arg(QString(mark), bookLine(book)));
        for (const LibraryDiff::Field &field : book.details)
            printLine(stdout, u"      %1: %2 → %3"_s.arg(field.field, shown(field.before), shown(field.after)));
        if (const QString notes = countsLine(book.notes); mark == u'~' && !notes.isEmpty())
            printLine(stdout, u"      annotations: "_s + notes);
    }
}

void printDiff(const SnapshotRestore::Compared &compared) {
    const LibraryDiff::Diff &diff = compared.diff;
    printLine(stdout, u"From %1 to %2:"_s.arg(named(compared.from), compared.to ? named(*compared.to) : u"now"_s));
    if (diff.empty()) {
        printLine(stdout, u"Nothing changed."_s);
        return;
    }
    printBooks(u"Books added"_s, u'+', diff.added);
    printBooks(u"Books removed"_s, u'-', diff.removed);
    printBooks(u"Books changed"_s, u'~', diff.changed);
    if (const QString notes = countsLine(diff.notes); !notes.isEmpty())
        printLine(stdout, u"Annotations: "_s + notes);
    const QString graph = countsLine({{u"things"_s, diff.things}, {u"relations"_s, diff.relations}, {u"facts"_s, diff.facts}});
    if (!graph.isEmpty())
        printLine(stdout, u"Knowledge graph: "_s + graph);
}

// The book `given` names: a global id, else a title the snapshot's copy has, else one the library has now.
[[nodiscard]] std::expected<QString, Failure> bookNamed(const Library &copy, const Library &live, const QString &given) {
    for (const Library *library : {&copy, &live})
        if (Spaces::localId(*library, u"book"_s, given) > 0)
            return given;
    for (const Library *library : {&copy, &live}) {
        QStringList found;
        for (const Book &book : library->books())
            if (book.meta.title.compare(given, Qt::CaseInsensitive) == 0)
                found << Spaces::bookId(*library, book.id);
        if (found.size() > 1)
            return std::unexpected(Failure(Failure::Code::SnapshotBookAmbiguous, {given}));
        if (found.size() == 1)
            return found.first();
    }
    return std::unexpected(Failure(Failure::Code::SnapshotBookNotFound, {given}));
}

// What putting the book back changes, from how it is now to how the snapshot has it; whether anything does.
[[nodiscard]] bool printPutBack(FILE *stream, const LibraryDiff::BookChange &change, BookRestore::Parts parts,
                                const Restic::Snapshot &snapshot) {
    const bool details = parts.details && !change.details.isEmpty();
    bool notes = false;
    QStringList said;
    for (const auto &[kind, count] : change.notes.asKeyValueRange()) {
        QStringList counted;
        if (count.added > 0)
            counted << u"%1 to come back"_s.arg(count.added);
        if (count.changed > 0)
            counted << u"%1 to change back"_s.arg(count.changed);
        if (count.removed > 0)
            counted << u"%1 made since, which stay"_s.arg(count.removed);
        notes = notes || count.added > 0 || count.changed > 0;
        said << kind + u": "_s + counted.join(u", "_s);
    }
    notes = notes && parts.annotations;
    if (!details && !notes) {
        printLine(stream, u"Nothing to put back: %1 is as %2 has it."_s.arg(change.title, named(snapshot)));
        return false;
    }
    printLine(stream, u"Putting back %1 as %2 has it:"_s.arg(bookLine(change), named(snapshot)));
    if (details)
        for (const LibraryDiff::Field &field : change.details)
            printLine(stream, u"  %1: %2 → %3"_s.arg(field.field, shown(field.before), shown(field.after)));
    if (notes)
        for (const QString &line : said)
            printLine(stream, u"  "_s + line);
    return true;
}

[[nodiscard]] bool confirmed(FILE *stream) {
    std::print(stream, "Put this back? A moment snapshot is taken first, to undo it with. [y/N] ");
    std::ignore = fflush(stream);
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly))
        return false;
    const QString answer = QString::fromUtf8(input.readLine()).trimmed();
    printLine(stream, {});
    return answer.startsWith(u'y', Qt::CaseInsensitive);
}

int restoreWhole(const QCommandLineParser &parser, const Snapshots &snapshots, const Library &library, const QString &id) {
    const auto snapshot = SnapshotRestore::find(snapshots, library, id);
    if (!snapshot)
        return fail(snapshot.error());
    const QString folder = parser.value(u"to"_s);
    if (const auto restored = SnapshotRestore::whole(snapshots, *snapshot, folder); !restored)
        return fail(restored.error());
    if (parser.isSet(u"json"_s)) {
        printJson({{u"snapshot"_s, SnapshotRestore::toJson(*snapshot)}, {u"library"_s, folder}});
        return 0;
    }
    printLine(stdout, u"The library as %1 holds it is restored into %2, a library of its own; the one in use is as it was. "
                      "omabook libraries --use %2 makes it the one in use."_s.arg(named(*snapshot), folder));
    return 0;
}

int restoreBook(const QCommandLineParser &parser, const Snapshots &snapshots, Library &library, const QString &id) {
    const auto snapshot = SnapshotRestore::find(snapshots, library, id);
    if (!snapshot)
        return fail(snapshot.error());
    const auto copy = SnapshotRestore::Copy::open(snapshots, *snapshot);
    if (!copy)
        return fail(copy.error());
    const auto book = bookNamed(copy->library(), library, parser.value(u"book"_s));
    if (!book)
        return fail(book.error());
    const bool only = parser.isSet(u"details"_s) || parser.isSet(u"annotations"_s);
    const BookRestore::Parts parts{.details = !only || parser.isSet(u"details"_s), .annotations = !only || parser.isSet(u"annotations"_s)};
    const auto change = LibraryDiff::book(library, copy->library(), *book);
    if (!change)
        return fail(Failure(Failure::Code::SnapshotBookNotFound, {*book}));
    // With --json the answer alone is on standard output.
    FILE *said = parser.isSet(u"json"_s) ? stderr : stdout;
    if (!printPutBack(said, *change, parts, *snapshot))
        return 0;
    if (!parser.isSet(u"yes"_s) && !confirmed(said)) {
        printLine(stderr, u"Nothing was put back."_s);
        return 1;
    }
    const auto done = SnapshotRestore::book(snapshots, library, *copy, *book, parts);
    if (!done)
        return fail(done.error());
    if (parser.isSet(u"json"_s)) {
        printJson(done->toJson());
        return 0;
    }
    printLine(stdout, u"Put back. To undo it: omabook snapshot restore %1 --book %2"_s.arg(done->undo.left(8), *book));
    return 0;
}

} // namespace

int runSnapshotRestoreCli(const QStringList &arguments) {
    QCommandLineParser parser;
    parser.setApplicationDescription(
        u"diff tells what changed in the library from a snapshot to another, or to now: the books added, removed and changed, "
        "detail by detail, and how many annotations and things of the knowledge graph. restore goes back to a snapshot: "
        "the whole library into a new folder (--to), never over the one in use, or one book's details and annotations into "
        "the library (--book), after showing what changes and asking, with a moment snapshot first to undo it with."_s);
    parser.addHelpOption();
    parser.addPositionalArgument(u"what"_s, u"diff or restore."_s, u"<diff|restore>"_s);
    parser.addPositionalArgument(u"snapshot"_s, u"The snapshot's id, or its first letters (omabook snapshot list)."_s);
    parser.addPositionalArgument(u"to"_s, u"diff: the later snapshot; now when not given."_s, u"[<snapshot>]"_s);
    parser.addOption({u"json"_s, u"Print the result as JSON."_s});
    parser.addOption({u"library"_s, u"Use the library in this folder instead of the current one, for this run only."_s, u"folder"_s});
    parser.addOption({u"to"_s, u"restore: the whole library, into this new or empty folder."_s, u"folder"_s});
    parser.addOption({u"book"_s, u"restore: this book, by its global id or its title."_s, u"book"_s});
    parser.addOption({u"details"_s, u"restore --book: its details (title, authors, ISBN, identifiers...)."_s});
    parser.addOption({u"annotations"_s, u"restore --book: its notes, highlights, bookmarks and ink. Both when neither is given."_s});
    parser.addOption({u"yes"_s, u"restore --book: put it back without asking."_s});
    QStringList rest = arguments;
    rest.removeAt(1);
    rest[0] = u"omabook snapshot"_s;
    parser.process(rest);
    const QStringList words = parser.positionalArguments();
    const QString what = words.value(0);
    const bool restoring = what == "restore"_L1;
    if (words.size() < 2 || words.size() > (restoring ? 2 : 3) || (restoring && parser.isSet(u"to"_s) == parser.isSet(u"book"_s))) {
        printLine(stderr, parser.helpText().trimmed());
        return 1;
    }
    const Snapshots snapshots(SnapshotPlace::here());
    if (!snapshots.ready())
        return fail(Failure(Failure::Code::SnapshotsNotSetUp));
    Library library(parser.isSet(u"library"_s) ? parser.value(u"library"_s) : Settings().libraryPath());
    // A library lost, or never here: its snapshots are found by its folder, and restored into a new one.
    if (restoring && parser.isSet(u"to"_s))
        return restoreWhole(parser, snapshots, library, words.at(1));
    library.onBringingUpToDate(LibraryMigrations::sayOnStandardError);
    if (QString error; !library.open(&error)) {
        printLine(stderr, error);
        return 1;
    }
    if (restoring)
        return restoreBook(parser, snapshots, library, words.at(1));
    const auto compared = SnapshotRestore::compare(snapshots, library, words.at(1), words.value(2));
    if (!compared)
        return fail(compared.error());
    if (parser.isSet(u"json"_s))
        printJson(compared->toJson());
    else
        printDiff(*compared);
    return 0;
}
