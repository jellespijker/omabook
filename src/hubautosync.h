#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <chrono>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>

#include "failure.h"

class QLockFile;
struct SyncSummary;

// Automatic sync with the hubs this computer is linked to (#191, ADR 0011):
// off until switched on for a hub (`omabook hub sync <hub> --auto on`, or the
// Hubs panel). Once on, the library in use syncs with the hub by itself: a
// little after it changed, once the window has been left alone that long; on
// a timer, whatever happened; and when the hub comes back into reach after it
// couldn't be reached. With several hubs (#196), what a sync with one brought
// is a change for the others, never for that one, so a change goes round the
// hubs once and comes to rest. Never while another sync of this computer's with a hub
// runs (hold()), and never on battery or a metered connection (SyncWait),
// which those who drive it ask before each.
//
// A sync the hub or the sync guard refuses (a library not paired, a breach of
// the guard's rules, an Omabook to update) isn't tried again: the refusal is
// kept, with what to do, for the panel to show and the desktop to be told of
// once, and automatic sync with that hub stays stopped until the person looks:
// a sync by hand goes through, or they switch it off and on again. A hub out
// of reach is no refusal: it is tried again in a while.
//
// What is switched on, when each hub last synced and how the last went are
// kept in a file in `folder` (beside the links, HubLinks), which the window
// and the background service (`omabook hub sync --service`) both read and
// write, so that a refusal told by one is never told again by the other. What
// happened in the window since is this one's own.
class HubAutoSync {
public:
    struct Times {
        std::chrono::seconds settle{std::chrono::minutes(2)}; // after a change, once the window was left alone this long
        std::chrono::seconds every{std::chrono::minutes(15)}; // after the last sync, whatever happened
        std::chrono::seconds again{std::chrono::minutes(2)};  // after a try that didn't reach the hub
    };

    // How it stands with one hub.
    struct Hub {
        bool on = false;
        QDateTime last;         // when the last sync that went through began
        QString refused;        // what the last was refused with, and what to do: tried no more until the person looks
        bool told = false;      // and the desktop was told of it
        bool unreached = false; // the last try didn't reach the hub
        QDateTime tried;        // when that was
    };

    explicit HubAutoSync(QString folder);
    HubAutoSync(QString folder, Times times);
    ~HubAutoSync();
    HubAutoSync(const HubAutoSync &) = delete;
    HubAutoSync &operator=(const HubAutoSync &) = delete;
    HubAutoSync(HubAutoSync &&) = delete;
    HubAutoSync &operator=(HubAutoSync &&) = delete;

    [[nodiscard]] Times times() const { return m_times; }
    [[nodiscard]] Hub hub(const QString &host) const;
    // Switched on or off for `host`; switched on, it is due at once, and a refusal kept is let go.
    [[nodiscard]] std::expected<void, Failure> turn(const QString &host, bool on);

    // The library changed at `now`.
    void changed(const QDateTime &now);
    // The person did something in the window at `now`: a change waits on.
    void touched(const QDateTime &now);
    // The network is back: every hub that wasn't reached is tried at once.
    void cameIntoReach();

    // Which of `hosts` to sync with at `now`, if any.
    [[nodiscard]] std::optional<QString> due(const QStringList &hosts, const QDateTime &now) const;
    // An automatic sync begins at `now`.
    void began(const QDateTime &now);
    // How the automatic sync with `host` begun last went, done at `now`; the
    // refusal to tell the desktop of, the first time it is seen.
    [[nodiscard]] std::optional<QString> done(const QString &host, const SyncSummary &summary, const QDateTime &now);
    // A sync by hand with `host`, begun at `began`, went through: the person
    // has seen to what was refused, and the timer goes on from then; what it
    // brought here is a change for the other hubs.
    void byHand(const QString &host, const QDateTime &began, bool broughtHere = false);

    // Held while a sync of this computer's with a hub runs, by whichever
    // Omabook runs it; null when another holds it, unless `wait`.
    [[nodiscard]] std::unique_ptr<QLockFile> hold(bool wait = false) const;

    // Whether a sync brought anything here: books, details, reading, collections, the graph, marks, plugins' files.
    [[nodiscard]] static bool brought(const SyncSummary &summary);
    // What a sync was refused with, and what to do, as the person reads it.
    [[nodiscard]] static QString refusal(const SyncSummary &summary);
    // The desktop's notification of a refusal by `host`: its title and its body.
    [[nodiscard]] static std::pair<QString, QString> notice(const QString &host, const QString &refused);

private:
    [[nodiscard]] std::map<QString, Hub> read() const;
    [[nodiscard]] std::expected<void, Failure> write(const std::map<QString, Hub> &hubs) const;
    [[nodiscard]] bool dueOne(const QString &host, const Hub &hub, const QDateTime &now) const;
    // When the library last changed for `host`: by the person, or by what a sync with another hub brought.
    [[nodiscard]] std::optional<QDateTime> changedFor(const QString &host) const;

    QString m_folder;
    Times m_times;
    std::optional<QDateTime> m_changed;
    QDateTime m_touched;
    QDateTime m_began;
    std::map<QString, QDateTime> m_brought; // when a sync with each hub last brought something
    std::set<QString> m_reachAgain;         // the hubs out of reach as the network came back
};
