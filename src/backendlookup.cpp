// Looking up a book's details across online catalogs and the AI, judging
// what each candidate brings, and applying the chosen one with its cover.

#include "backend.h"

#include <QFileInfo>
#include <QJsonArray>
#include <algorithm>

#include "formats.h"
#include "graph.h"

QVariantList Backend::candidates() const {
    QVariantList list;
    const auto book = m_library ? m_library->book(m_lookUpId) : std::nullopt;
    for (const MetaCandidate &candidate : m_candidates) {
        const BookMeta &meta = candidate.meta;
        // What taking it would do: the fields it fills in, and the ones it
        // changes, so a result can be judged without comparing by eye.
        const MetaDiff diff = book ? whatItBrings(book->meta, book->hasCover, candidate) : MetaDiff();
        QVariantList changes;
        for (const MetaDiff::Change &change : diff.changes)
            changes << QVariantMap{{"field", change.field}, {"from", change.from}, {"to", change.to}};
        list << QVariantMap{
            {"source", candidate.source},
            {"title", meta.title},
            {"authors", meta.authorText()},
            {"series", meta.series},
            {"publisher", meta.publisher},
            {"published", meta.published},
            {"description", meta.description},
            {"tags", meta.tags.join(", ")},
            {"cover", candidate.coverUrl},
            {"why", candidate.why},
            {"adds", diff.adds},
            {"changes", changes},
        };
    }
    return list;
}

QVariantList Backend::lookUpSources() const {
    QVariantList list;
    for (const SourceState &state : m_sourceStates)
        list << QVariantMap{
            {"name", state.name}, {"fields", state.fields}, {"asking", state.asking}, {"found", state.found}, {"problem", state.problem}};
    return list;
}

// The first pages of a book, which is where it names itself.
QString Backend::openingPages(qint64 id) const {
    QString opening;
    const auto book = m_library->book(id);
    for (const QString &format : book ? book->formats : QStringList()) {
        if (!Formats::readable(format))
            continue;
        if (const auto document = Formats::open(m_library->formatPath(id, format)))
            for (int section = 0; section < document->count() && opening.size() < 6000; ++section)
                opening += document->text(section) + "\n\n";
        break;
    }
    return opening.left(6000);
}

void Backend::lookUp(qint64 id) {
    stopLookUp();
    const auto book = m_library ? m_library->book(id) : std::nullopt;
    if (!book)
        return;
    m_lookUpId = id;
    m_sources = m_catalogs(this);
    // The AI goes last: it is handed what the catalogs found, to judge
    // which of it is really this book.
    m_lookUpAi = m_aiFactory(nullptr);
    if (m_lookUpAi) {
        m_aiSource = new AiMetadataSource(m_lookUpAi.get(), QFileInfo(m_library->formatPath(id)).fileName(), openingPages(id), this);
        m_sources << m_aiSource;
    }
    for (int index = 0; index < m_sources.size(); ++index) {
        const MetadataSource *source = m_sources.at(index);
        m_sourceStates.append({source->name(), source->fields(), true, 0, {}});
        connect(source, &MetadataSource::found, this,
                [this, index](const QList<MetaCandidate> &candidates) { sourceAnswered(index, candidates, {}); });
        connect(source, &MetadataSource::failed, this,
                [this, index](const Failure &problem) { sourceAnswered(index, {}, problem.text()); });
    }
    m_pendingSources = static_cast<int>(m_sources.size());
    emit candidatesChanged();
    for (MetadataSource *source : std::as_const(m_sources))
        if (source != m_aiSource)
            source->search(book->meta);
    askAiWhenCatalogsAreDone();
}

void Backend::askAiWhenCatalogsAreDone() {
    if (!m_aiSource || m_aiAsked || m_pendingSources != 1)
        return;
    const auto book = m_library ? m_library->book(m_lookUpId) : std::nullopt;
    if (!book)
        return;
    m_aiAsked = true;
    m_aiSource->setFound(m_candidates);
    m_aiSource->search(book->meta);
}

void Backend::sourceAnswered(int index, const QList<MetaCandidate> &candidates, const QString &problem) {
    if (index < 0 || index >= m_sourceStates.size() || !m_sourceStates[index].asking)
        return;
    SourceState &state = m_sourceStates[index];
    state.asking = false;
    state.found = static_cast<int>(candidates.size());
    state.problem = problem;
    // The AI's judgement is the recommendation, so it leads the list.
    if (m_sources.value(index) == m_aiSource)
        m_candidates = candidates + m_candidates;
    else
        m_candidates << candidates;
    if (!problem.isEmpty())
        m_searchProblem += (m_searchProblem.isEmpty() ? "" : "\n") + problem;
    m_pendingSources = std::max(0, m_pendingSources - 1);
    emit candidatesChanged();
    askAiWhenCatalogsAreDone();
    if (m_pendingSources == 0) {
        QJsonArray found;
        for (const MetaCandidate &candidate : std::as_const(m_candidates))
            found << candidate.toJson();
        m_scripts->emitEvent("lookup.finished", {{"id", m_lookUpId}, {"candidates", found}});
    }
}

void Backend::applyCandidate(int index) {
    const auto before = m_library ? m_library->book(m_lookUpId) : std::nullopt;
    if (!before || index < 0 || index >= m_candidates.size())
        return;
    const MetaCandidate candidate = m_candidates.at(index);
    const qint64 id = m_lookUpId;
    BookMeta meta = before->meta;
    meta.apply(candidate.meta);
    QString error;
    if (!m_library->setMeta(id, meta, &error))
        say(error);
    Graph(*m_library).bookNode(id);
    reload();
    // Into its file as well where the setting says so, once its cover is here.
    if (candidate.coverUrl.isValid())
        fetchCoverFor(id, candidate.coverUrl);
    else
        writeDetailsWhenAsked(id, m_settings->writeDetails());
}

void Backend::fetchCoverFor(qint64 id, const QUrl &url) {
    fetchCover(&m_network, url, this, [this, id](const QImage &cover) {
        // Catalogs answer a missing cover with a one-pixel picture.
        if (m_library && cover.width() > 50 && m_library->setCover(id, cover)) {
            reload();
            writeDetailsWhenAsked(id, m_settings->writeDetails());
        }
    });
}
