#include "graphjobs.h"

#include <QEventLoop>
#include <QEventLoopLocker>
#include <QPointer>
#include <QThreadPool>
#include <QtConcurrent>

#include "aicall.h"
#include "backend.h"

namespace {

// One question to the window's model, waited for on the worker's own thread.
// The worker holds its own copy of the factory: the backend may go while it
// works (GraphJobs is its child, so it waits for the worker only once the
// backend's members are gone).
[[nodiscard]] GraphBuild::Asker askerFor(Backend::AiFactory factory) {
    return [factory = std::move(factory)](const AiRequest &request, QString *answer, QString *error) {
        std::unique_ptr<AiProvider> provider = factory(error);
        if (!provider)
            return false;
        AiCall call(std::move(provider));
        QEventLoop loop;
        bool ok = false;
        QObject::connect(&call, &AiCall::done, &loop, [&](const QString &text) {
            *answer = text;
            ok = true;
            loop.quit();
        });
        QObject::connect(&call, &AiCall::failed, &loop, [&](const QString &problem) {
            *error = problem;
            loop.quit();
        });
        call.ask(request);
        loop.exec();
        return ok;
    };
}

} // namespace

GraphJobs::GraphJobs(Backend *backend) : QObject(backend), m_backend(backend) {}

GraphJobs::~GraphJobs() {
    // The worker holds its own library; it is asked to stop and waited for,
    // so nothing of this object is used after it is gone.
    m_stop->store(true);
    m_work.waitForFinished();
}

void GraphJobs::consider(const QString &which, qint64 selectedId, bool whole) {
    Library *library = m_backend->library();
    if (!library || m_busy)
        return;
    GraphBatch::Choice choice;
    choice.whole = whole;
    if (which == "selected" && selectedId > 0) {
        choice.books << selectedId;
    } else if (which == "shown") {
        for (int row = 0; row < m_backend->books()->count(); ++row)
            choice.books << m_backend->books()->idAt(row);
    } else if (which == "read") {
        choice.books = GraphBatch::readBooks(*library);
    }
    m_choice = choice;
    m_planning = true;
    m_summary.clear();
    emit planChanged();
    emit busyChanged();

    // Working it out opens every book, which takes a moment for a shelf of
    // them, so it happens beside the window on a library of its own.
    const int asked = ++m_asked;
    const QString root = library->root();
    const QPointer<GraphJobs> self(this);
    QThreadPool::globalInstance()->start([self, asked, root, choice] {
        Library own(root);
        const GraphBatch::Plan planned = own.open() ? GraphBatch::plan(own, choice) : GraphBatch::Plan();
        QVariantList skipped;
        for (const GraphBatch::Plan::Item &item : planned.items)
            if (!item.skipped.isEmpty())
                skipped << QVariantMap{{"title", item.title}, {"why", item.skipped}};
        const QVariantMap plan{{"books", planned.books}, {"requests", planned.requests},
                               {"tokens", planned.tokens()}, {"skipped", skipped}};
        QMetaObject::invokeMethod(qApp, [self, asked, plan] {
            if (!self || asked != self->m_asked)
                return;
            self->m_plan = plan;
            self->m_planning = false;
            emit self->planChanged();
        });
    });
}

void GraphJobs::start() {
    Library *library = m_backend->library();
    if (!library || m_busy || m_planning || m_plan.value("books").toInt() == 0)
        return;
    m_busy = true;
    m_stop->store(false);
    m_progress = {{"index", 0}, {"total", m_plan.value("books")}, {"title", QString()}, {"requests", 0}, {"nodes", 0}, {"edges", 0}};
    emit busyChanged();
    emit progressChanged();

    const QString root = library->root();
    const GraphBatch::Choice choice = m_choice;
    const std::shared_ptr<std::atomic_bool> stop = m_stop;
    const GraphBuild::Asker ask = askerFor(m_backend->aiFactory());
    const QPointer<GraphJobs> self(this);
    m_work = QtConcurrent::run([self, root, choice, stop, ask] {
        // Closing the window meanwhile leaves the app running until the
        // request under way is answered and laid into the graph.
        const QEventLoopLocker locker;
        Library own(root);
        GraphBatch::Done done;
        if (own.open())
            done = GraphBatch::run(own, choice, ask, [stop] { return stop->load(); },
                                   [self](int index, int total, const QString &title, const GraphBatch::Done &soFar) {
                                       const QVariantMap progress{{"index", index + 1}, {"total", total}, {"title", title},
                                                                  {"requests", soFar.requests}, {"nodes", soFar.nodes},
                                                                  {"edges", soFar.edges}};
                                       QMetaObject::invokeMethod(qApp, [self, progress] {
                                           if (!self)
                                               return;
                                           self->m_progress = progress;
                                           emit self->progressChanged();
                                           emit self->grown();
                                       });
                                   });
        const QVariantMap summary{{"books", done.books}, {"requests", done.requests}, {"nodes", done.nodes},
                                  {"edges", done.edges}, {"stopped", done.stopped}, {"problems", done.problems}};
        QMetaObject::invokeMethod(qApp, [self, summary] {
            if (!self)
                return;
            self->m_summary = summary;
            self->m_busy = false;
            self->m_plan.clear();
            emit self->planChanged();
            emit self->busyChanged();
            emit self->grown();
        });
    });
}

void GraphJobs::stop() { m_stop->store(true); }
