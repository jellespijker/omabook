#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <expected>
#include <functional>

#include "failure.h"
#include "remote.h"
#include "sync.h"

class HubLinks;
class QJsonObject;

// A hub, as the other side of a sync (#188, ADR 0011): the sync two computers
// do (syncLibrary), with what it asks of the other side said through this
// computer's session with its hub (HubSession) as the hub's request `sync`
// (HubSync), where it would run a command over ssh. The hub runs no command
// for it: a book's file goes there a piece at a time into a folder of the
// sync's own, and comes back the same way.
//
// Begun, a sync names the hub's library (the target's folder, else its one)
// and this one's format, and the hub refuses a library of another format,
// saying which Omabook to update; a dry run writes nothing there. A library
// the hub's isn't paired with is refused too, saying how to pair (#214).
class HubRemote : public Remote {
public:
    // Asks the hub one line and answers with its answer, one line; empty
    // once the session ended. The hello was said and answered before.
    using Ask = std::function<QByteArray(const QByteArray &line)>;
    using Progress = std::function<void(const QString &title)>;

    // `target` is the hub as ssh reaches it, or "<hub>:<library>".
    HubRemote(const QString &target, Ask ask);
    ~HubRemote() override;
    HubRemote(const HubRemote &) = delete;
    HubRemote &operator=(const HubRemote &) = delete;
    HubRemote(HubRemote &&) = delete;
    HubRemote &operator=(HubRemote &&) = delete;

    // A library the hub's isn't paired with is refused here, before anything else, unless `pair` (#214).
    [[nodiscard]] std::expected<void, Failure> begin(const Library &library, bool dryRun, bool pair = false);
    // Lets the hub know it is over: what was sent for it goes there.
    void end();

    [[nodiscard]] bool hub() const override { return true; }
    Result omabook(const QStringList &arguments, const QString &inputFile = {}) const override;
    Result run(const QString &command, const QString &inputFile = {}, const QString &outputFile = {}) const override;
    bool fetch(const QString &remoteFile, const QString &localFile) const override;
    [[nodiscard]] std::expected<void, QString> send(const QString &localFile, const QString &remoteFile) const override;
    [[nodiscard]] std::expected<QString, QString> makeFolder() const override;
    void dropFolder(const QString &folder) const override;

    // A whole sync with the hub through `ask`: begun, synced as with any computer, and ended.
    [[nodiscard]] static SyncSummary sync(Library &library, const QString &target, const Ask &ask, const Progress &progress = {},
                                          const SyncOptions &options = {});
    // A sync with the hub `target`, one `links` linked this computer to, over a session opened for it. A sync
    // that paired this computer, making it another person, is followed by saying so to the hub (#283).
    [[nodiscard]] static SyncSummary syncLinked(Library &library, const HubLinks &links, const QString &target,
                                                const Progress &progress = {}, const SyncOptions &options = {});
    // A sync with `target`: through the session when it is a hub this computer is linked to (HubLinks),
    // else with the computer over ssh (`ssh`, as Remote takes it), as ever.
    [[nodiscard]] static SyncSummary syncWith(Library &library, const QString &target, const Progress &progress = {},
                                              const SyncOptions &options = {}, const QStringList &ssh = {});

private:
    [[nodiscard]] std::expected<QJsonObject, Failure> step(QJsonObject request) const;

    Ask m_ask;
    QString m_library;
    QString m_visit;
};
