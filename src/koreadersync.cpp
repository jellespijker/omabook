#include "koreadersync.h"

#include <QHash>
#include <cstdint>
#include <optional>
#include <utility>

#include "koreaderbooks.h"
#include "kosyncclient.h"
#include "library.h"

QJsonObject KoreaderSync::Summary::toJson() const { return {{"taken", taken}, {"sent", sent}, {"books", books}}; }

namespace {

enum class Outcome : std::uint8_t { Same, Taken, Sent };

using Files = QList<std::pair<QString, KoreaderBooks::File>>;

// What the server has of each of a book's files, and the newest a device said.
struct Heard {
    QHash<QString, KoreaderRecord> known;
    std::optional<std::pair<KoreaderRecord, KoreaderBooks::File>> newest;
};

[[nodiscard]] std::expected<Heard, Failure> hear(KosyncClient &client, const Files &files, const QString &us) {
    Heard heard;
    for (const auto &[document, file] : files) {
        const auto remote = client.progress(document);
        if (!remote)
            return std::unexpected(remote.error());
        if (!*remote)
            continue;
        heard.known.insert(document, **remote);
        if ((*remote)->deviceId != us && (!heard.newest || (*remote)->timestamp > heard.newest->first.timestamp))
            heard.newest = std::pair{**remote, file};
    }
    return heard;
}

// Whether the server has this very place from here already.
[[nodiscard]] bool alreadyThere(const QHash<QString, KoreaderRecord> &known, const KoreaderRecord &record, const QString &us) {
    const auto had = known.constFind(record.document);
    return had != known.cend() && had->deviceId == us && had->progress == record.progress
           && qFuzzyCompare(1 + had->percentage, 1 + record.percentage);
}

// One book: the newest place a device said taken, or the place here sent
// for every file when it is the newer.
[[nodiscard]] std::expected<Outcome, Failure> syncBook(const Library &library, KoreaderBooks &books, KosyncClient &client, qint64 book) {
    const QString us = KoreaderBooks::deviceId(library);
    const Files files = books.filesOf(book);
    const auto heard = hear(client, files, us);
    if (!heard)
        return std::unexpected(heard.error());
    const auto &newest = heard->newest;
    if (newest && books.take(newest->second, newest->first))
        return Outcome::Taken;
    const qint64 here = library.progress(book).updated;
    if (here <= 0 || (newest && newest->first.timestamp >= here))
        return Outcome::Same;
    bool sent = false;
    for (const auto &[document, file] : files) {
        const auto record = books.recordOf(file, document, us);
        if (!record || alreadyThere(heard->known, *record, us))
            continue;
        if (const auto put = client.put(*record); !put)
            return std::unexpected(put.error());
        sent = true;
    }
    return sent ? Outcome::Sent : Outcome::Same;
}

} // namespace

std::expected<KoreaderSync::Summary, Failure> KoreaderSync::run(Library &library, KosyncClient &client, const QList<qint64> &only) {
    if (const auto allowed = client.authorize(); !allowed)
        return std::unexpected(allowed.error());
    KoreaderBooks books(library);
    QList<qint64> chosen = only;
    if (chosen.isEmpty()) {
        for (const Book &book : library.books()) {
            if (!book.formats.isEmpty())
                chosen << book.id;
        }
    }
    Summary summary;
    for (const qint64 book : std::as_const(chosen)) {
        const auto outcome = syncBook(library, books, client, book);
        if (!outcome)
            return std::unexpected(outcome.error());
        ++summary.books;
        summary.taken += *outcome == Outcome::Taken ? 1 : 0;
        summary.sent += *outcome == Outcome::Sent ? 1 : 0;
    }
    return summary;
}
