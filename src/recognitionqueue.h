#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <chrono>
#include <expected>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>

#include "recognition.h"
#include "recognitionroute.h"

class Library;
class Settings;

// The recognition service (#84): what is to be recognised waits its turn here,
// one at a time, in the background. Each goes the ways RecognitionRoute gives,
// one after the other while a way has no such service; what a way recognised
// is kept in the change log (RecognitionStore) and what was kept is answered
// without asking anyone. While the server can't be reached (offline, or the
// computer it runs on asleep) the queue waits and tries again, ever less
// often, until it answers or the one who asked cancels.
class RecognitionQueue : public QObject {
    Q_OBJECT

public:
    struct Request {
        Recognition::Input input;
        // Whose data the result is kept with: a plugin's id (PluginData).
        QString owner;
        // A hosted server may be asked, where the settings allow one at all.
        bool hosted = false;
        // Recognised anew even though a result is kept.
        bool again = false;
    };
    // Called once for each request: its result, or why there is none, in
    // plain words. A cancelled one is told so too.
    using Done = std::function<void(const std::expected<Recognition::Result, QString> &)>;
    // Called each time a request starts to wait for a server, with why.
    using Waiting = std::function<void(const QString &why)>;
    using Route = std::function<std::expected<RecognitionRoute::Ways, QString>(Recognition::Kind, bool hosted)>;
    using LibraryOf = std::function<Library *()>;

    // The ways from `route`; results kept in the library `library` answers,
    // none when it answers none.
    RecognitionQueue(Route route, LibraryOf library, QObject *parent = nullptr);
    ~RecognitionQueue() override;
    Q_DISABLE_COPY_MOVE(RecognitionQueue)

    // The ways the settings say, as they say them when a request's turn comes.
    [[nodiscard]] static Route routeFor(const Settings &settings);

    // Queues a request; its number, for cancel().
    int submit(Request request, Done done, Waiting waiting = {});
    // Stops a request, under way or waiting its turn; false when there is no such one.
    bool cancel(int job);
    [[nodiscard]] qsizetype pending() const { return static_cast<qsizetype>(m_jobs.size()); }
    // How long the first wait for a server is, and the longest; each doubles the one before.
    void setRetry(std::chrono::milliseconds first, std::chrono::milliseconds most);
    // Whether the last recognition of this kind failed because the server
    // lacks a model for it (one that reads pictures, or transcribes), and the
    // ways are still the ones it went (#133): it would fail so again.
    // Why, in plain words, when it did; none otherwise.
    [[nodiscard]] std::optional<QString> lacking(Recognition::Kind kind, bool hosted) const;

private:
    struct Job;

    void next();
    void step(Job &job);
    void resolved(Job &job);
    void failed(Job &job, const Recognition::Failure &failure);
    void finish(const std::expected<Recognition::Result, QString> &outcome);
    [[nodiscard]] QString signatureNow(Recognition::Kind kind, bool hosted) const;

    Route m_route;
    LibraryOf m_library;
    std::list<std::unique_ptr<Job>> m_jobs; // the first is the one under way
    int m_nextJob = 1;
    bool m_running = false;
    QTimer m_retry;
    std::chrono::milliseconds m_firstWait;
    std::chrono::milliseconds m_longestWait;
    // By kind: the ways (RecognitionRoute::signature) that lacked a model,
    // and why, until one of that kind is recognised.
    struct Lacking {
        QString signature;
        QString why;
    };
    std::map<Recognition::Kind, Lacking> m_lacking;
};
