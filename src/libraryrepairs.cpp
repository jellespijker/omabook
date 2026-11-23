#include "libraryrepairs.h"

#include <QSqlQuery>

#include "booklinks.h"
#include "changelog.h"
#include "changelogfields.h"
#include "library.h"
#include "log.h"
#include "replication.h"
#include "strayidentifiers.h"

namespace {

// NOLINTBEGIN(bugprone-throwing-static-initialization,cert-err58-cpp): constant table of SQL statements.
const QList<QStringList> repairs{
    // The same-as links an older Omabook made between books that are not one
    // book (#158, format 43): judged once, and kept apart from then on.
    {"INSERT OR REPLACE INTO library_state (key, value) VALUES ('judge_links', '1')"},
    // A book's identifiers of one kept apart (#164, format 44): removed once.
    {"INSERT OR REPLACE INTO library_state (key, value) VALUES ('unstray_identifiers', '1')"},
    // The details of books got by a sync before #210, which this computer
    // never wrote as its own (#213, format 45): written once.
    {"INSERT OR REPLACE INTO library_state (key, value) VALUES ('echo_taken_books', '1')"},
};
// NOLINTEND(bugprone-throwing-static-initialization,cert-err58-cpp)

// Whether the repair of `key` is due, and from now on not.
[[nodiscard]] bool takeDue(const Library &library, const QString &key) {
    QSqlQuery due = ChangeLog::query(library, "SELECT 1 FROM library_state WHERE key = ?", {key});
    const bool isDue = due.next();
    due.finish();
    if (isDue)
        ChangeLog::exec(library, "DELETE FROM library_state WHERE key = ?", {key});
    return isDue;
}

// The repair of format 43 (#158): the links that are not of one book, each
// recorded as such once (BookLinks::record), and said.
void judgeLinks(const Library &library, const std::function<void(const QString &)> &say) {
    if (!takeDue(library, "judge_links"))
        return;
    const QStringList apart = BookLinks::record(library, Replication::operations(library));
    if (apart.isEmpty())
        return;
    qCInfo(lcLibrary).noquote() << apart.size() << "links an older Omabook made between books that are not one book were dropped";
    if (say)
        say(QString("An older Omabook took %1 for one book; a sync keeps them apart now: %2.")
                .arg(apart.size() == 1 ? QString("1 pair of books") : QString("%1 pairs of books").arg(apart.size()), apart.join("; ")));
}

// The repair of format 44 (#164): the identifiers books have of a book kept
// apart from them, removed once (StrayIdentifiers::remove), and said.
void unstrayIdentifiers(Library &library, const std::function<void(const QString &)> &say) {
    if (!takeDue(library, "unstray_identifiers"))
        return;
    const auto [removed, doubtful] = StrayIdentifiers::remove(library, Replication::operations(library), false);
    const auto count = [](qsizetype n) { return n == 1 ? QString("1 identifier") : QString("%1 identifiers").arg(n); };
    if (!removed.isEmpty())
        qCInfo(lcLibrary).noquote() << removed.size() << "identifiers books had of a book kept apart from them were removed";
    if (!say)
        return;
    if (!removed.isEmpty())
        say(QString("Removed %1 that a book had of another an older Omabook took it for: %2.")
                .arg(count(removed.size()), removed.join("; ")));
    if (!doubtful.isEmpty())
        say(QString("Kept %1 that a book shares with another an older Omabook took it for, as whose they "
                    "are can't be told: %2.")
                .arg(count(doubtful.size()), doubtful.join("; ")));
}

// The repair of format 45 (#213): what this computer took of its books from
// another and never wrote as its own, written once (ChangeLogFields::echoTaken).
// Nothing a person would see changes, so nothing is said.
void echoTakenBooks(const Library &library) {
    if (!takeDue(library, "echo_taken_books"))
        return;
    if (const qsizetype books = ChangeLogFields::echoTaken(library); books > 0)
        qCInfo(lcLibrary).noquote() << "the details of" << books << "books got by a sync were written as this computer's own";
}

} // namespace

const QList<QStringList> &LibraryRepairs::steps() { return repairs; }

void LibraryRepairs::run(Library &library, const std::function<void(const QString &)> &say) {
    judgeLinks(library, say);
    unstrayIdentifiers(library, say);
    echoTakenBooks(library);
}
