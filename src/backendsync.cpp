// The window's side of syncing (sync.h): the summary it says when it is
// done. Kept apart from the rest of Backend, which it only touches through
// its jobs, reload() and say().

#include "backend.h"

#include <QCoreApplication>
#include <QEventLoopLocker>
#include <QPointer>
#include <QThreadPool>

#include "hubremote.h"
#include "sync.h"

namespace {

QStringList syncStatusLines(const SyncSummary &summary) {
    QStringList lines;
    if (!summary.error.isEmpty())
        lines << summary.error;
    if (summary.received > 0)
        lines << Backend::tr("%Ln books received", "", summary.received);
    if (summary.sent > 0)
        lines << Backend::tr("%Ln books sent", "", summary.sent);
    if (summary.placesTaken + summary.placesSent > 0)
        lines << Backend::tr("%Ln reading places updated", "", summary.placesTaken + summary.placesSent);
    if (summary.detailsTaken + summary.detailsSent > 0)
        lines << Backend::tr("%Ln books' details updated", "", summary.detailsTaken + summary.detailsSent);
    if (summary.removedHere > 0)
        lines << Backend::tr("%Ln books removed here, to the trash", "", summary.removedHere);
    if (summary.removedThere > 0)
        lines << Backend::tr("%Ln books removed on the other computer, to the trash", "", summary.removedThere);
    if (summary.graphTaken > 0)
        lines << Backend::tr("%Ln graph changes taken", "", summary.graphTaken);
    if (summary.graphSent > 0)
        lines << Backend::tr("%Ln graph changes sent", "", summary.graphSent);
    if (summary.annotationsTaken + summary.annotationsSent > 0)
        lines << Backend::tr("%Ln bookmarks and highlights updated", "", summary.annotationsTaken + summary.annotationsSent);
    if (summary.detailsUndecided > 0)
        lines << Backend::tr("%Ln books have other details on the other computer, and neither side knows which "
                             "are newer. omabook sync --details mine or --details theirs settles them.",
                             "", summary.detailsUndecided);
    if (lines.isEmpty() && summary.problems.isEmpty())
        lines << Backend::tr("Both libraries were already in step.");
    lines << summary.problems.mid(0, 3);
    return lines;
}

} // namespace

// Syncing talks to another computer for as long as its books take to cross,
// so it runs on a worker with its own connection to the library. A hub this
// computer is linked to is synced with through its session (#188).
void Backend::sync() {
    const QString target = m_settings->syncTarget();
    if (!m_library || busy())
        return;
    if (target.isEmpty()) {
        say(tr("Name the computer to sync with in the settings first."));
        return;
    }
    ++m_jobs;
    setStatus(tr("Syncing with %1…").arg(target.section(':', 0, 0)));
    const QString root = m_library->root();
    const QStringList ssh = m_ssh;
    const QPointer<Backend> self(this);
    QThreadPool::globalInstance()->start([self, root, target, ssh] {
        const QEventLoopLocker locker;
        SyncSummary summary;
        Library library(root);
        if (!library.open(&summary.error))
            summary.problems.clear();
        else
            summary = HubRemote::syncWith(
                library, target,
                [self](const QString &title) {
                    QMetaObject::invokeMethod(
                        qApp,
                        [self, title] {
                            if (self)
                                self->setStatus(title);
                        },
                        Qt::QueuedConnection);
                },
                {}, ssh);
        QMetaObject::invokeMethod(
            qApp,
            [self, summary] {
                if (!self)
                    return;
                --self->m_jobs;
                self->setStatus({});
                self->reload();
                self->say(syncStatusLines(summary).join("\n"));
            },
            Qt::QueuedConnection);
    });
}
