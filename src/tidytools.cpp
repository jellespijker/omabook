#include <QJsonArray>
#include <QList>
#include <QSet>
#include <QStringList>
#include <algorithm>
#include <memory>

#include "bookops.h"
#include "curation.h"
#include "library.h"
#include "mcp.h"
#include "mcpbulk.h"

// Applying a tidy-up an agent chose (tidy_propose, which only looks, is with
// the library's tools in mcp.cpp): a change to many books at once, so one of
// more than MomentSnapshot::bulkBooks takes a moment snapshot first (#208).

namespace {

using namespace Mcp;

// The books the proposals change: each one's book, and a duplicate's other.
qsizetype booksProposed(const QJsonObject &arguments) {
    QSet<qint64> books;
    for (const auto &given : arguments.value("proposals").toArray()) {
        const Proposal proposal = BookOps::proposalFromJson(given.toObject());
        for (const qint64 id : {proposal.bookId, proposal.otherId})
            if (id > 0)
                books.insert(id);
    }
    return books.size();
}

Result apply(const QJsonObject &arguments) {
    QString error;
    const std::shared_ptr<Library> library = Mcp::library(&error);
    if (!library)
        return Result::fail(error);
    QList<Proposal> proposals;
    for (const auto &given : arguments.value("proposals").toArray()) {
        const Proposal proposal = BookOps::proposalFromJson(given.toObject());
        if (!QStringList{"duplicate", "author", "series"}.contains(proposal.kind) || proposal.bookId <= 0)
            return Result::fail("Each proposal needs a \"kind\" (duplicate, author or series) and the book's \"id\", "
                                "as tidy_propose returns them.");
        proposals << proposal;
    }
    // Merges last: they take books away that other proposals may name.
    std::ranges::stable_sort(proposals,
                             [](const Proposal &a, const Proposal &b) { return a.kind != "duplicate" && b.kind == "duplicate"; });
    int applied = 0;
    QStringList problems;
    for (const Proposal &proposal : std::as_const(proposals)) {
        QString why;
        if (Curation::apply(*library, proposal, &why))
            ++applied;
        else
            problems << why;
    }
    return Result::ok({{"applied", applied}, {"problems", QJsonArray::fromStringList(problems)}});
}

// NOLINTNEXTLINE(bugprone-throwing-static-initialization): the tool registry is filled at start-up (mcp.h), as mcpcommands.cpp fills it.
const bool registered =
    registerTool({"tidy_apply", "Apply chosen tidy-ups",
                  "Makes the changes of the proposals given, which are objects as tidy_propose returned them. A "
                  "\"duplicate\" folds one book into another and moves what is left of it to the trash, so ask the "
                  "user first. A change to many books takes a moment snapshot first, and says it as \"undo\".",
                  schema({{"proposals", list("The proposals to apply.", object("One proposal from tidy_propose."))}}, {"proposals"}),
                  Destructive, McpBulk::snapshotFirst(booksProposed, apply)});

} // namespace
