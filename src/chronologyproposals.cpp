#include "chronologyproposals.h"

#include <QVariantMap>

#include "chronologybranch.h"
#include "chronologymodel.h"
#include "graph.h"

QObject *ChronologyProposals::browser() const { return m_browser; }

void ChronologyProposals::setBrowser(QObject *browser) {
    auto *given = qobject_cast<ChronologyBrowser *>(browser);
    if (given == m_browser)
        return;
    if (m_browser)
        m_browser->disconnect(this);
    m_browser = given;
    if (m_browser)
        connect(m_browser, &ChronologyBrowser::changed, this, &ChronologyProposals::read);
    read();
    emit browserChanged();
}

Library *ChronologyProposals::library() const {
    Library *window = m_browser ? m_browser->changing() : nullptr;
    return window ? window : m_library;
}

void ChronologyProposals::read() {
    QVariantList list;
    if (m_browser)
        for (const auto &part : m_browser->reading().parts)
            for (const Chronology::Proposal &proposal : part.chronicle.proposals)
                // A shared graph's things are numbered below 0 here.
                if (proposal.timeline.id > 0)
                    list << QVariantMap{{"timeline", proposal.timeline.id},
                                        {"name", proposal.timeline.name},
                                        {"from", proposal.split.parent.name},
                                        {"at", proposal.split.at.name}};
    if (list == m_list)
        return;
    m_list = list;
    emit changed();
}

bool ChronologyProposals::decide(qint64 timeline, bool accept) {
    const Library *books = library();
    if (!books)
        return false;
    Graph graph = Graph::ofThing(*books, timeline);
    if (!Chronology::decide(graph, timeline, accept))
        return false;
    if (m_browser)
        m_browser->refresh();
    return true;
}
