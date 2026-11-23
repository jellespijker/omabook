#pragma once

#include <QJsonObject>
#include <QString>
#include <QtTypes>
#include <chrono>
#include <expected>
#include <map>
#include <memory>

#include "failure.h"

class HubKeeper;
class HubRequests;

// A computer's sync with its hub, the hub's side (#188, ADR 0011): the hub is
// a peer like any other computer, and a sync with it is the sync two computers
// do (sync.h), driven by the computer. What that sync asks of the other side
// over ssh, the computer asks here through its session (HubRemote), as the
// request `sync`, a step at a time; the session sets "computer" to its key's.
//
//   {"step": "begin", "library": <name, or none for the hub's one>, "format": <the computer's library's>, "dry_run": false,
//    "device": <the computer's library's>}
//     → {"visit": <id>, "library": <name>, "format": <the hub's>}
//   {"step": "omabook", "visit": <id>, "arguments": [...]}  → {"answer": <what it printed>} (HubSyncSteps)
//   {"step": "folder", "visit": <id>}                       → {"folder": <its name>}
//   {"step": "put", "visit": <id>, "file": <folder>/<name>, "at": <offset>, "data": <base64>} → {"size": <now>}
//   {"step": "fetch", "visit": <id>, "file": <path>, "at": <offset>}     → {"data": <base64>, "size": <the whole>}
//   {"step": "drop", "visit": <id>, "folder": <name>}      and {"step": "end", "visit": <id>}
//
// A visit is one computer's, never another's; what it is sent stays in its own
// folders and goes with it; it fetches only the files of the library's books
// and their covers, and the plugins' files the hub keeps for it (#212), when
// its device is one of the hub library's own computers. Every step is done through the keeper (ADR 0013), and a
// sync takes its moment snapshot (#181) once, just before its first write. A library of another format than the computer's doesn't sync:
// both sides say which Omabook to update. A dry run writes nothing at all.
class HubSync {
public:
    // How long a sync may stand without a step before the hub lets it go.
    static constexpr std::chrono::hours idle{1};
    // The most of a file one step carries, before base64: a line well within HubDaemon::longestLine.
    static constexpr qsizetype piece = qsizetype{512} * 1024;

    // Its visits' folders are made in `scratch`: the system's temporary folder when empty.
    explicit HubSync(QString scratch = {});
    ~HubSync();
    HubSync(const HubSync &) = delete;
    HubSync &operator=(const HubSync &) = delete;
    HubSync(HubSync &&) = delete;
    HubSync &operator=(HubSync &&) = delete;

    // Adds the request `sync` to `requests`, answered by this, which outlives them.
    void addTo(HubRequests &requests);

    [[nodiscard]] std::expected<QJsonObject, Failure> answer(const QJsonObject &asked, HubKeeper &keeper);

private:
    struct Visit;

    [[nodiscard]] std::expected<QJsonObject, Failure> begin(const QJsonObject &asked, HubKeeper &keeper);
    [[nodiscard]] std::expected<QJsonObject, Failure> step(Visit &visit, const QJsonObject &asked, HubKeeper &keeper);
    // Lets go of the visits no step came for in `idle`.
    void forgetIdle();

    QString m_scratch;
    std::map<QString, std::unique_ptr<Visit>> m_visits;
};
