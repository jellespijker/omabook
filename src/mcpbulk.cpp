#include "mcpbulk.h"

#include <QJsonArray>
#include <memory>
#include <utility>

#include "library.h"
#include "momentsnapshot.h"
#include "snapshots.h"

namespace {

using namespace Qt::StringLiterals;

[[nodiscard]] QJsonObject undoOf(const QString &id) {
    const QString shortId = id.left(8);
    return {{u"snapshot"_s, id},
            {u"how"_s, u"A moment snapshot holds the library as it was before this. omabook snapshot diff %1 shows what "
                       "changed since; omabook snapshot restore %1 --book <book> puts one book back, or --to <folder> the "
                       "whole library into a new folder."_s.arg(shortId)}};
}

// The answer's notes with `note` after them.
void note(QJsonObject &data, const QString &note) {
    const QString notes = data.value(u"notes"_s).toString();
    data.insert(u"notes"_s, notes.isEmpty() ? note : notes + u'\n' + note);
}

} // namespace

Mcp::Handler McpBulk::snapshotFirst(Count count, Mcp::Handler handler) {
    return [count = std::move(count), handler = std::move(handler)](const QJsonObject &arguments) -> Mcp::Result {
        if (!MomentSnapshot::isBulk(count(arguments)))
            return handler(arguments);
        QString error;
        const std::shared_ptr<Library> library = Mcp::library(&error);
        if (!library)
            return Mcp::Result::fail(error);
        const auto first = MomentSnapshot::beforeBulk(Snapshots(SnapshotPlace::here()), *library);
        if (!first)
            return Mcp::Result::fail(u"Nothing was changed: no moment snapshot of the library could be taken first. "_s
                                     + first.error().english());
        Mcp::Result result = handler(arguments);
        // What failed half way through may have changed some books: the snapshot holds them as they were.
        if (result.failed() && !first->undo.isEmpty())
            return Mcp::Result::fail(result.error
                                     + u" A moment snapshot taken first holds the library as it was: %1."_s.arg(first->undo.left(8)));
        if (result.failed())
            return result;
        if (!first->undo.isEmpty())
            result.data.insert(u"undo"_s, undoOf(first->undo));
        if (first->unsnapshot)
            note(result.data, u"No moment snapshot could be taken first, so this went on without one: "_s + first->unsnapshot->english());
        return result;
    };
}

McpBulk::Count McpBulk::listed(const QString &key) {
    return [key](const QJsonObject &arguments) { return arguments.value(key).toArray().size(); };
}
