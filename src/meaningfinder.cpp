#include "meaningfinder.h"

#include <QVariantMap>
#include <QtConcurrent>

#include "document.h"
#include "library.h"
#include "log.h"
#include "reader.h"
#include "settings.h"

using namespace Qt::StringLiterals;

namespace {

// The library in that folder, opened on the thread that asks.
[[nodiscard]] std::expected<std::unique_ptr<Library>, Failure> opened(const QString &root) {
    auto library = std::make_unique<Library>(root);
    QString why;
    if (!library->open(&why))
        return std::unexpected(Failure::said(why));
    return library;
}

// What the Find says of an index by how fresh it is.
[[nodiscard]] QString stateOf(MeaningSearch::Freshness freshness) {
    switch (freshness) {
    case MeaningSearch::Freshness::Fresh:
        return u"ready"_s;
    case MeaningSearch::Freshness::Stale:
        return u"stale"_s;
    case MeaningSearch::Freshness::None:
        break;
    }
    return u"none"_s;
}

} // namespace

MeaningFinder::MeaningFinder(QObject *parent) : QObject(parent) {
    connect(&m_look, &QFutureWatcher<Look>::finished, this, &MeaningFinder::looked);
    connect(&m_index, &QFutureWatcher<Made>::finished, this, &MeaningFinder::indexed);
    connect(&m_search, &QFutureWatcher<Hits>::finished, this, &MeaningFinder::searched);
}

MeaningFinder::~MeaningFinder() {
    m_stop.request_stop();
    m_look.waitForFinished();
    m_index.waitForFinished();
    m_search.waitForFinished();
}

QObject *MeaningFinder::reader() const { return m_reader; }

void MeaningFinder::setReader(QObject *reader) {
    auto *given = qobject_cast<Reader *>(reader);
    if (given == m_reader)
        return;
    if (m_reader)
        disconnect(m_reader, nullptr, this, nullptr);
    m_reader = given;
    if (m_reader)
        connect(m_reader, &Reader::bookChanged, this, &MeaningFinder::look);
    emit readerChanged();
    look();
}

void MeaningFinder::setState(const QString &state) {
    m_state = state;
    emit changed();
}

void MeaningFinder::look() {
    const Library *library = m_reader ? m_reader->library() : nullptr;
    if (!library || m_reader->bookId() <= 0 || m_look.isRunning() || m_index.isRunning())
        return;
    m_bookId = m_reader->bookId();
    m_look.setFuture(QtConcurrent::run([root = library->root(), bookId = m_bookId] {
        const Settings settings;
        Look look{.route = Embeddings::route(settings), .freshness = MeaningSearch::Freshness::None, .due = false};
        const auto library = opened(root);
        if (!look.route || !library)
            return look;
        look.freshness = MeaningSearch::freshness(**library, bookId, look.route->model);
        const auto state = MeaningIndex::state(**library, bookId);
        look.due = look.freshness != MeaningSearch::Freshness::Fresh
                   && ((state && state->wanted) || (settings.meaningAll() && !(*library)->formatFiles(bookId).isEmpty()));
        return look;
    }));
}

void MeaningFinder::looked() {
    const Look seen = m_look.result();
    if (!m_reader || m_reader->bookId() != m_bookId) {
        look();
        return;
    }
    m_route = seen.route ? std::optional(*seen.route) : std::nullopt;
    m_problem = seen.route ? QString() : seen.route.error().text();
    m_where = seen.route ? tr("%1 on %2 (%3)").arg(seen.route->model, seen.route->server.name, seen.route->server.host()) : QString();
    setState(seen.route ? stateOf(seen.freshness) : u"refused"_s);
    if (seen.due && m_bookId != m_leftAlone)
        start();
}

void MeaningFinder::index() {
    m_leftAlone = 0;
    start();
}

void MeaningFinder::start() {
    const Library *library = m_reader ? m_reader->library() : nullptr;
    if (!library || !m_route || m_reader->bookId() <= 0 || m_index.isRunning())
        return;
    const Embeddings::Route route = *m_route;
    m_bookId = m_reader->bookId();
    m_stop = std::stop_source();
    m_progress = 0;
    setState(u"indexing"_s);
    m_index.setFuture(QtConcurrent::run([this, root = library->root(), bookId = m_bookId, route, stop = m_stop.get_token()] -> Made {
        const auto library = opened(root);
        if (!library)
            return std::unexpected(library.error());
        return MeaningSearch::index(**library, route, bookId, stop, [this](MeaningSearch::Progress progress) {
            // Queued to the window's thread; dropped if this has gone, which waits for the work first.
            QMetaObject::invokeMethod(this, [this, progress] {
                m_progress = progress.total > 0 ? static_cast<double>(progress.done) / progress.total : 0;
                emit changed();
            });
        });
    }));
}

void MeaningFinder::cancel() { m_stop.request_stop(); }

void MeaningFinder::indexed() {
    const Made made = m_index.result();
    if (made) {
        m_problem.clear();
        setState(u"ready"_s);
    } else {
        m_leftAlone = m_bookId;
        m_problem = made.error().code() == Failure::Code::MeaningCancelled ? QString() : made.error().text();
        if (!m_problem.isEmpty())
            qCWarning(lcLibrary).noquote() << "book" << m_bookId << "can't be indexed by its meaning:" << m_problem;
        setState(u"none"_s);
    }
    look();
}

void MeaningFinder::find(const QString &words) {
    const Library *library = m_reader ? m_reader->library() : nullptr;
    if (!library || !m_route || m_state != "ready"_L1 || words.simplified().size() < 2 || m_search.isRunning())
        return;
    const MeaningSearch::Question question{.words = words.simplified(), .books = {m_reader->bookId()}, .whole = m_whole, .limit = 10};
    m_search.setFuture(QtConcurrent::run([root = library->root(), route = *m_route, question] -> Hits {
        const auto library = opened(root);
        if (!library)
            return std::unexpected(library.error());
        return MeaningSearch::search(**library, route, question);
    }));
    emit changed();
}

QString MeaningFinder::whereIs(int section) const {
    if (m_reader->kind() == "pages"_L1)
        return tr("Page %1").arg(section + 1);
    const std::shared_ptr<const Document> document = m_reader->document();
    QString title;
    int best = -1;
    for (const TocEntry &entry : document ? document->toc() : QList<TocEntry>())
        if (entry.section <= section && entry.section > best) {
            best = entry.section;
            title = entry.title;
        }
    return title;
}

void MeaningFinder::searched() {
    emit changed();
    const Hits hits = m_search.result();
    if (!hits) {
        m_problem = hits.error().text();
        emit changed();
        return;
    }
    QVariantList results;
    for (const MeaningIndex::Hit &hit : *hits) {
        if (!m_reader || hit.bookId != m_reader->bookId())
            continue;
        const QString where = whereIs(hit.from.section);
        results << QVariantMap{{"section", hit.from.section},
                               {"fraction", hit.from.fraction},
                               {"title", where.isEmpty() ? tr("By meaning") : tr("By meaning · %1").arg(where)},
                               {"snippet", hit.text},
                               {"meant", true},
                               {"by", MeaningSearch::toJson(hit).value("by"_L1).toString()}};
    }
    emit found(results);
}
