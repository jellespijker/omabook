#pragma once

#include <QHash>
#include <QJsonObject>
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

// Merges a book's details field by field using each field's HLC.
[[nodiscard]] MergedBookDetails mergeBookDetails(const Library &library, const Book &here, const QJsonObject &theirs, bool current,
                                                 SyncOptions::Details choice);

} // namespace SyncManifest
