#pragma once

#include <QJsonObject>
#include <QString>
#include <QtTypes>
#include <expected>
#include <utility>

#include "failure.h"

// Where a hub keeps the snapshots another of the person's hubs, or one of
// their computers, sends it (#196, ADR 0014: a Destination): one restic
// repository for each of the sender's destinations, in a folder of that
// computer's own (<folder>/<computer>/<repository>), laid out as restic lays
// out a repository on a disk, so the person can restore from it there too.
// What is kept is encrypted by restic with the sender's password, which this
// hub never has: it keeps the snapshots, and can't read them.
//
// Asked through the computer's session (HubSession), never a shell: the
// request `store`, answered by the session itself, beside the daemon, as no
// library is written (so two hubs that keep each other's snapshots, copying
// at the same hour, never wait on each other's daemons). The computer is the
// session's key's, whatever the line says; a request names the repository and
// one of restic's files in it (config, or keys|locks|snapshots|index|data/<id>),
// never a path:
//
//   {"op": "create"}                                   → {}
//   {"op": "stat", "path": <file>}                     → {"exists": bool, "size": <bytes>}
//   {"op": "list", "type": <keys|locks|...|data>}      → {"files": [{"name", "size"}]}
//   {"op": "get", "path": <file>, "at": <offset>, "length": <at most piece>} → {"data": <base64>, "size": <the whole>}
//   {"op": "put", "path": <file>, "at": <offset>, "data": <base64>, "last": bool} → {"size": <so far>}
//   {"op": "remove", "path": <file>}                   → {"removed": bool}
//
// A file comes a piece at a time, in order, into a file of its own beside it,
// and takes its place only with its last piece, so a copy cut short leaves no
// half a file where restic would read one.
class HubStore {
public:
    // The most of a file one request carries, before base64: a line well within HubDaemon::longestLine.
    static constexpr qsizetype piece = qsizetype{512} * 1024;

    explicit HubStore(QString folder) : m_folder(std::move(folder)) {}

    // <state>/stores on a hub.
    [[nodiscard]] static QString in(const QString &state);

    [[nodiscard]] std::expected<QJsonObject, Failure> answer(const QJsonObject &asked, const QString &computer) const;

private:
    QString m_folder;
};
