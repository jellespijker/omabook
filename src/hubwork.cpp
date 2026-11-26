#include "hubwork.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QScopeGuard>
#include <QTimer>
#include <algorithm>
#include <ranges>
#include <span>
#include <tuple>
#include <utility>

#include "aicall.h"
#include "aicapability.h"
#include "airegistry.h"
#include "bookops.h"
#include "formats.h"
#include "graphbatch.h"
#include "graphbuilder.h"
#include "graphlinks.h"
#include "hubclaims.h"
#include "hubkeeper.h"
#include "hubrequests.h"
#include "library.h"
#include "log.h"
#include "settings.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

// How many of the jobs done the status tells of.
constexpr qsizetype told = 10;

// The hub's settings, local from the start (ADR 0008): a hub's config.json is
// never carried over from the older keys, which would allow hosted answers.
void startLocal(const QString &file) {
    if (Settings settings(file); !settings.hasCapabilities())
        settings.setCapabilities(Capabilities::fresh());
}

// How often a request under way looks whether the job was cancelled.
constexpr auto looking = std::chrono::milliseconds(200);

// One request answered by the model the settings give quick work, waited
// for, unless `stop` comes first: then it is let go of, unanswered.
[[nodiscard]] std::expected<QString, Failure> asked(const Settings &settings, const AiRequest &request, const std::atomic_bool &stop) {
    auto provider = AiRegistry::answers(settings);
    if (!provider)
        return std::unexpected(provider.error());
    AiCall call(std::move(*provider));
    QEventLoop loop;
    std::expected<QString, Failure> answer = std::unexpected(Failure(Failure::Code::HubWorkCancelled));
    bool over = false; // it may be told before the loop runs
    QObject::connect(&call, &AiCall::done, &loop, [&](const QString &text) {
        answer = text;
        over = true;
        loop.quit();
    });
    QObject::connect(&call, &AiCall::failed, &loop, [&](const Failure &failure) {
        answer = std::unexpected(failure);
        over = true;
        loop.quit();
    });
    QTimer watch;
    QObject::connect(&watch, &QTimer::timeout, &loop, [&] {
        if (!stop)
            return;
        call.cancel();
        answer = std::unexpected(Failure(Failure::Code::HubWorkCancelled));
        loop.quit();
    });
    watch.start(looking);
    call.ask(request);
    if (!over)
        loop.exec();
    return answer;
}

struct Built {
    int requests = 0, nodes = 0, edges = 0;
    bool complete = false;
};

// The graph of a book built whole in `library`, then compared with the other
// books: the same for the copy the model is asked over and for the library
// its answers are laid into, so both ask the same things in the same order.
[[nodiscard]] Built buildWhole(Library &library, const Document &document, qint64 book, const GraphBuild::Asker &ask,
                               const std::function<void(int requests)> &said = {}) {
    const GraphPlace end{book, document.count() - 1, 1.0};
    const GraphBuild::Built built = GraphBuild::build(library, document, book, end, ask, [&said](const GraphBuild::Built &now) {
        if (said)
            said(now.requests);
    });
    Built done{.requests = built.requests, .nodes = built.nodes, .edges = built.edges, .complete = built.complete};
    if (!built.complete)
        return done;
    const GraphLinks::Connected connected = GraphLinks::connect(library, book, ask);
    done.requests += connected.asked ? 1 : 0;
    done.nodes += connected.topics;
    done.edges += connected.links + 2 * connected.topics;
    return done;
}

// The file a book is read from, and its hash; nothing for a book with none Omabook reads.
[[nodiscard]] std::optional<std::pair<QString, QString>> fileOf(const Library &library, qint64 book) {
    QString error;
    const QString path = BookOps::readableFile(library, book, &error);
    if (path.isEmpty())
        return std::nullopt;
    const QList<FormatFile> files = library.formatFiles(book);
    const auto file = std::ranges::find_if(
        files, [&path](const FormatFile &one) { return QFileInfo(one.path).canonicalFilePath() == QFileInfo(path).canonicalFilePath(); });
    return std::pair{path, file != files.end() && !file->hash.isEmpty() ? file->hash : Library::hashFile(path)};
}

[[nodiscard]] QJsonValue placeJson(const std::optional<GraphPlace> &place) {
    return place ? QJsonValue(QJsonObject{{"section", place->section}, {"fraction", place->fraction}}) : QJsonValue();
}

} // namespace

QString HubWork::Job::key() const { return QStringList{id, hash, Capabilities::name(Capability::Quick), model}.join(u'|'); }

HubWork::HubWork(HubKeeper &keeper, QString stateFolder, QString settingsFile, QString name)
    : m_keeper(keeper), m_file(QDir(stateFolder).filePath(u"work.json"_s)), m_scratch(QDir(stateFolder).filePath(u"work"_s)),
      m_settings(std::move(settingsFile)), m_name(std::move(name)) {
    read();
}

HubWork::~HubWork() {
    // The worker's copy of a library is its own to close: let it finish first.
    m_stop = true;
    if (m_thread)
        m_thread->wait();
}

bool HubWork::busy() const {
    const std::scoped_lock lock(m_lock);
    return m_running.has_value();
}

HubSchedule::Job HubWork::job() {
    return [this](HubKeeper &) { return look(); };
}

bool HubWork::dueNow(const QString &key, const QDateTime &now) const {
    const auto kept = m_kept.find(key);
    if (kept == m_kept.end())
        return true;
    // Done or skipped, never again; failed or cancelled, after a while.
    std::chrono::hours wait{0};
    if (kept->second.outcome == "failed"_L1)
        wait = retry;
    else if (kept->second.outcome == "cancelled"_L1)
        wait = rest;
    return wait.count() > 0 && kept->second.at.addSecs(std::chrono::duration_cast<std::chrono::seconds>(wait).count()) <= now;
}

std::expected<QList<HubWork::Job>, Failure> HubWork::due(const QString &model) {
    QList<Job> jobs;
    const QDateTime now = QDateTime::currentDateTime();
    const auto found = m_keeper.whole([&](std::span<Library *const> libraries) -> std::expected<void, Failure> {
        for (const Library *library : libraries)
            for (const Book &book : library->books()) {
                if (book.formats.isEmpty())
                    continue;
                const auto file = fileOf(*library, book.id);
                if (!file)
                    continue;
                Job job{.library = QFileInfo(library->root()).fileName(),
                        .book = book.id,
                        .id = Spaces::globalId(*library, u"book"_s, book.id),
                        .title = book.meta.title,
                        .hash = file->second,
                        .model = model};
                if (dueNow(job.key(), now))
                    jobs << std::move(job);
            }
        return {};
    });
    if (!found)
        return std::unexpected(found.error());
    return jobs;
}

std::expected<void, Failure> HubWork::look() {
    if (busy())
        return {};
    startLocal(m_settings);
    const Settings settings(m_settings);
    const CapabilityStatus quick = AiRegistry::status(settings, Capability::Quick);
    {
        const std::scoped_lock lock(m_lock);
        m_unavailable = quick.available ? QJsonObject() : quick.toJson();
    }
    // Nothing is asked of a model the hub may not use: the status says why.
    if (!quick.available)
        return {};
    const auto jobs = due(quick.model.isEmpty() ? quick.provider : quick.provider + u'/' + quick.model);
    if (!jobs)
        return std::unexpected(jobs.error());
    for (const auto &[index, job] : std::views::enumerate(*jobs)) {
        const auto started = prepare(job);
        if (!started)
            continue;
        {
            const std::scoped_lock lock(m_lock);
            m_waiting = static_cast<int>(jobs->size() - index - 1);
        }
        return start(*started);
    }
    const std::scoped_lock lock(m_lock);
    m_waiting = 0;
    return {};
}

std::optional<HubWork::Started> HubWork::prepare(const Job &job) {
    std::optional<Started> started;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const auto planned = m_keeper.write(job.library, HubKeeper::Moment::None, [&](Library &library) -> std::expected<QJsonObject, Failure> {
        // Another of the person's hubs builds it (#196): its graph comes by the sync.
        if (HubClaims::elsewhere(library, job.id, now))
            return QJsonObject();
        const GraphBatch::Plan plan = GraphBatch::plan(library, {.books = {job.book}, .whole = true, .again = false});
        const GraphBatch::Plan::Item item = plan.items.value(0);
        const auto file = fileOf(library, job.book);
        if (!item.skipped.isEmpty() || !file) {
            note(job, u"skipped"_s, item.skipped);
            return QJsonObject();
        }
        if (const auto claimed = HubClaims::claim(library, job.id, now); !claimed)
            return std::unexpected(claimed.error());
        started = Started{.job = job, .path = file->first, .before = GraphBuild::builtTo(library, job.book), .total = item.requests};
        GraphBuilt::mark(library, job.book, says(GraphBuilt::State::Building));
        return QJsonObject();
    });
    if (!planned)
        note(job, u"failed"_s, planned.error().english());
    return started;
}

std::expected<void, Failure> HubWork::start(const Started &started) {
    auto copy = m_keeper.copy(started.job.library, m_scratch);
    if (!copy) {
        note(started.job, u"failed"_s, copy.error().english());
        return std::unexpected(copy.error());
    }
    m_copy = std::move(*copy);
    m_stop = false;
    {
        const std::scoped_lock lock(m_lock);
        m_running = started;
        m_since = QDateTime::currentDateTime();
        m_done = 0;
    }
    qCInfo(lcHub).noquote() << "Building the graph of" << started.job.title << "in" << started.job.library << "with" << started.job.model;
    m_thread.reset(QThread::create([this, started] {
        const Result result = run(started);
        QMetaObject::invokeMethod(&m_context, [this, started, result] { finish(started, result); }, Qt::QueuedConnection);
    }));
    m_thread->start();
    return {};
}

HubWork::Result HubWork::run(const Started &started) {
    Result result;
    // The copy is opened, and closed, on this thread, whose connection it is.
    const auto closed = qScopeGuard([this] { m_copy.reset(); });
    QString error;
    if (!m_copy->open(&error)) {
        result.failure = Failure::said(error);
        return result;
    }
    auto document = Formats::opened(started.path);
    if (!document) {
        result.failure = document.error();
        return result;
    }
    const Settings settings(m_settings);
    const GraphBuild::Asker ask = [&](const AiRequest &request, QString *answer, QString *why) {
        if (m_stop) {
            result.cancelled = true;
            *why = Failure(Failure::Code::HubWorkCancelled).text();
            return false;
        }
        const auto got = asked(settings, request, m_stop);
        if (!got) {
            result.failure = got.error();
            *why = got.error().text();
            return false;
        }
        result.answers << *got;
        *answer = *got;
        return true;
    };
    const Built built = buildWhole(*m_copy, **document, started.job.book, ask, [this](int requests) {
        const std::scoped_lock lock(m_lock);
        m_done = requests;
    });
    result.cancelled = result.cancelled || m_stop;
    if (!built.complete && !result.failure && !result.cancelled)
        result.failure = Failure(Failure::Code::HubWorkUnfinished, {started.job.title});
    return result;
}

std::expected<QJsonObject, Failure> HubWork::lay(const Started &started, const QStringList &answers) {
    // After a moment snapshot: a whole book's graph is a change that touches much (#208).
    return m_keeper.write(started.job.library, HubKeeper::Moment::Bulk, [&](Library &library) -> std::expected<QJsonObject, Failure> {
        // Another hub's later claim came by a sync meanwhile (#196): that one's graph is the one kept.
        if (HubClaims::elsewhere(library, started.job.id, QDateTime::currentDateTimeUtc()))
            return std::unexpected(Failure(Failure::Code::HubWorkClaimed, {started.job.title}));
        // A sync brought the same book's graph meanwhile: the answers were to other questions.
        if (GraphBuild::builtTo(library, started.job.book) != started.before)
            return std::unexpected(Failure(Failure::Code::HubWorkChanged, {started.job.title}));
        auto document = Formats::opened(started.path);
        if (!document)
            return std::unexpected(document.error());
        qsizetype next = 0;
        const GraphBuild::Asker again = [&](const AiRequest &, QString *answer, QString *why) {
            if (next >= answers.size()) {
                *why = Failure(Failure::Code::HubWorkChanged, {started.job.title}).text();
                return false;
            }
            *answer = answers.at(next++);
            return true;
        };
        const Library::Batch batch(library);
        const Built built = buildWhole(library, **document, started.job.book, again);
        GraphBuilt::Mark mark = says(GraphBuilt::State::Built);
        if (const auto upTo = GraphBuild::builtTo(library, started.job.book))
            mark.to = upTo->inBook();
        GraphBuilt::mark(library, started.job.book, mark);
        return QJsonObject{{"requests", built.requests}, {"nodes", built.nodes}, {"edges", built.edges}};
    });
}

void HubWork::finish(const Started &started, const Result &result) {
    m_thread->wait();
    m_thread.reset();
    QDir(m_scratch).removeRecursively();
    bool goOn = false;
    bool done = false;
    if (result.cancelled) {
        note(started.job, u"cancelled"_s, Failure(Failure::Code::HubWorkCancelled).english());
    } else if (result.failure) {
        note(started.job, u"failed"_s, result.failure->english());
    } else if (const auto laid = lay(started, result.answers); !laid) {
        note(started.job, u"failed"_s, laid.error().english());
        goOn = laid.error().code() == Failure::Code::HubWorkChanged || laid.error().code() == Failure::Code::HubWorkClaimed;
    } else {
        qCInfo(lcHub).noquote() << "The graph of" << started.job.title << "took in" << laid->value("nodes"_L1).toInt() << "things and"
                                << laid->value("edges"_L1).toInt() << "ties.";
        note(started.job, u"done"_s);
        goOn = done = true;
    }
    // Not built here after all: a computer may build it as it is read.
    if (!done)
        std::ignore =
            m_keeper.write(started.job.library, HubKeeper::Moment::None, [&](Library &library) -> std::expected<QJsonObject, Failure> {
                GraphBuilt::mark(library, started.job.book, says(GraphBuilt::State::Stopped));
                return QJsonObject();
            });
    {
        const std::scoped_lock lock(m_lock);
        m_running.reset();
    }
    // On to the next book; after a failure, the schedule's next round.
    if (goOn)
        if (const auto next = look(); !next)
            qCWarning(lcHub).noquote() << "The hub's work can't go on:" << next.error().english();
}

bool HubWork::cancel() {
    if (!busy())
        return false;
    m_stop = true;
    return true;
}

void HubWork::note(const Job &job, const QString &outcome, const QString &said) {
    if (outcome != "done"_L1 && outcome != "skipped"_L1)
        qCWarning(lcHub).noquote() << "The graph of" << job.title << "in" << job.library << outcome << "-" << said;
    m_kept.insert_or_assign(job.key(), Kept{.library = job.library,
                                            .id = job.id,
                                            .title = job.title,
                                            .outcome = outcome,
                                            .said = said,
                                            .at = QDateTime::currentDateTime()});
    write();
}

GraphBuilt::Mark HubWork::says(GraphBuilt::State state) const {
    return {.state = state, .by = m_name, .hub = true, .at = QDateTime::currentDateTime(), .to = {}};
}

QJsonObject HubWork::status() const {
    QList<const Kept *> latest;
    for (const auto &[key, kept] : m_kept)
        latest << &kept;
    std::ranges::sort(latest, [](const Kept *one, const Kept *other) { return one->at > other->at; });
    QJsonArray last;
    for (const Kept *kept : latest | std::views::take(told))
        last.append(QJsonObject{{"library", kept->library},
                                {"book", kept->id},
                                {"title", kept->title},
                                {"outcome", kept->outcome},
                                {"at", kept->at.toString(Qt::ISODate)},
                                {"said", kept->said}});
    const std::scoped_lock lock(m_lock);
    QJsonObject status{{"waiting", m_waiting}, {"last", last}};
    if (!m_unavailable.isEmpty())
        status.insert("unavailable"_L1, m_unavailable);
    if (m_running)
        status.insert("running"_L1, QJsonObject{{"library", m_running->job.library},
                                                {"book", m_running->job.id},
                                                {"title", m_running->job.title},
                                                {"kind", "graph"},
                                                {"capability", Capabilities::name(Capability::Quick)},
                                                {"model", m_running->job.model},
                                                {"done", std::min(m_done, m_running->total)},
                                                {"total", m_running->total},
                                                {"from", placeJson(m_running->before)},
                                                {"started", m_since.toString(Qt::ISODate)}});
    return status;
}

void HubWork::addTo(HubRequests &requests) {
    requests.add(u"work"_s, [this](const QJsonObject &, HubKeeper &) -> std::expected<QJsonObject, Failure> { return status(); });
    requests.add(u"work-cancel"_s, [this](const QJsonObject &, HubKeeper &) -> std::expected<QJsonObject, Failure> {
        return QJsonObject{{"cancelled", cancel()}};
    });
}

void HubWork::read() {
    QFile file(m_file);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject jobs = QJsonDocument::fromJson(file.readAll()).object();
    for (auto job = jobs.begin(); job != jobs.end(); ++job) {
        const QJsonObject kept = job.value().toObject();
        m_kept.insert_or_assign(job.key(), Kept{.library = kept.value("library"_L1).toString(),
                                                .id = kept.value("book"_L1).toString(),
                                                .title = kept.value("title"_L1).toString(),
                                                .outcome = kept.value("outcome"_L1).toString(),
                                                .said = kept.value("said"_L1).toString(),
                                                .at = QDateTime::fromString(kept.value("at"_L1).toString(), Qt::ISODateWithMs)});
    }
}

void HubWork::write() const {
    QJsonObject jobs;
    for (const auto &[key, kept] : m_kept)
        jobs.insert(key, QJsonObject{{"library", kept.library},
                                     {"book", kept.id},
                                     {"title", kept.title},
                                     {"outcome", kept.outcome},
                                     {"said", kept.said},
                                     {"at", kept.at.toString(Qt::ISODateWithMs)}});
    QSaveFile file(m_file);
    if (!QDir().mkpath(QFileInfo(m_file).absolutePath()) || !file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(jobs).toJson()) < 0
        || !file.commit())
        qCWarning(lcHub).noquote() << "What the hub's work has done can't be kept in" << m_file << "; it looks again at its next start.";
}
