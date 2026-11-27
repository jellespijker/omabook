#pragma once

#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QtTypes>
#include <functional>
#include <optional>

#include "sync.h"

// Manifest HLC enrichment, wire-compatibility conversion for 0.1.5.2 peers,
// and field-by-field book detail merging (ADR 0001).
namespace SyncManifest {

struct MergedBookDetails {
    BookMeta meta;
    bool takeFields = false;
    bool sendFields = false;
    bool takeCover = false;
    bool sendCover = false;
    bool undecided = false;
    qint64 takeHlc = 0;
    qint64 sendHlc = 0;
};

void observeManifest(const Library &library, const QJsonObject &manifest);

// The names, as removals are logged (Spaces::removalGlobalId), that this
// computer's own log says it removed at some time, by hand or by a sync: of a
// book it holds and added since another removed it, that makes it one added
// again on purpose (#290), where one it never removed is only one it added
// not knowing.
[[nodiscard]] QSet<QString> removedByThisComputer(const Library &library);

[[nodiscard]] qint64 theirDetailsHlc(const QJsonObject &theirs);
[[nodiscard]] qint64 myDetailsHlc(const Library &library, const Book &book);
[[nodiscard]] qint64 theirProgressHlc(const QJsonObject &theirs);
[[nodiscard]] qint64 myProgressHlc(const Library &library, qint64 bookId, const Progress &place);
[[nodiscard]] qint64 lastChangeHlc(const Library &library, const Book &book);
[[nodiscard]] qint64 lastChangeHlc(const QJsonObject &theirs);
[[nodiscard]] std::optional<qint64> localRemovalHlc(const Library &library, const QString &key);
[[nodiscard]] QHash<QString, qint64> remoteRemovalHlcs(const QJsonObject &manifest);

// Plain seconds for an older peer without `"log": 1`, or the packed HLC when
// the other side speaks the log.
[[nodiscard]] qint64 wireTimestamp(qint64 hlc, bool peerHasLog) noexcept;
[[nodiscard]] Progress progressFrom(const QJsonObject &object);
[[nodiscard]] QStringList progressArguments(qint64 id, const Progress &progress);
[[nodiscard]] std::optional<qint64> removedWhen(const QStringList &hashes, const QStringList &keys,
                                                const std::function<std::optional<qint64>(const QString &)> &at);

// Merges a book's details field by field using each field's HLC, this side's
// as its log says them, theirs as their manifest does.
[[nodiscard]] MergedBookDetails mergeBookDetails(const Library &library, const Book &here, const QJsonObject &theirs, bool current,
                                                 SyncOptions::Details choice);
// The rule itself: of each field that differs, the newer one (by `mine`'s and
// `theirs`' "field_hlc", from their details time), or neither where the times
// are the same and `choice` says nothing; the cover too when `covers` differ.
[[nodiscard]] MergedBookDetails decideDetails(const Book &here, const QJsonObject &mine, const QJsonObject &theirs, bool current,
                                              SyncOptions::Details choice, bool covers);

} // namespace SyncManifest
