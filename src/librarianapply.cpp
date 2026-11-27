// The window's proposals as the person chooses them, and applying those, with
// a moment snapshot of the library first where snapshots are set up (#282).

#include <QtConcurrent>
#include <algorithm>
#include <tuple>

#include "backend.h"
#include "librarian.h"
#include "snapshots.h"

void Librarian::add(const QList<Proposal> &proposals) {
    // A book left undecided is shown, for the person; there is nothing to apply.
    // One that is the person's to judge (Proposal::waits) waits to be ticked.
    for (const Proposal &proposal : proposals)
        m_entries.append({proposal, proposal.kind != "undecided" && !proposal.waits()});
    emit proposalsChanged();
}

void Librarian::setChosen(int index, bool chosen) {
    if (index < 0 || index >= m_entries.size())
        return;
    m_entries[index].chosen = chosen && m_entries.at(index).proposal.kind != "undecided";
    emit proposalsChanged();
}

void Librarian::applyChosen() {
    const Library *library = m_backend->library();
    if (!library || m_busy)
        return;
    QList<Proposal> proposals;
    // Details and spellings first, then the merges, which take books away.
    for (const bool merges : {false, true})
        for (const Entry &entry : std::as_const(m_entries))
            if (entry.chosen && (entry.proposal.kind == "duplicate") == merges)
                proposals << entry.proposal;
    const Snapshots snapshots(SnapshotPlace::here());
    // Never set up: nothing to take. With their password gone, stage() refuses and says why (#296).
    if (!snapshots.chosen()) {
        apply(proposals);
        return;
    }
    // The database is copied here, between writes; restic keeps it aside from the window.
    const auto staged = snapshots.stage(*library);
    if (!staged) {
        refuseUnsnapshot(staged.error());
        return;
    }
    setBusy(true, tr("Taking a moment snapshot first…"));
    const int asked = ++m_applyAsked;
    std::ignore = QtConcurrent::run([snapshots, what = *staged] {
                      return snapshots.keep(what, SnapshotReason::Tidy);
                  }).then(this, [this, asked, proposals, root = library->root()](const std::expected<Snapshots::Taken, Failure> &taken) {
        // Closed or looked over again meanwhile, or another library: nothing is changed.
        if (asked != m_applyAsked)
            return;
        setBusy(false);
        if (!taken)
            refuseUnsnapshot(taken.error());
        else if (m_backend->library() && m_backend->library()->root() == root)
            apply(proposals);
    });
}

void Librarian::refuseUnsnapshot(const Failure &why) {
    m_backend->sayOutcome(tr("Nothing was changed: no moment snapshot of the library could be taken first. %1").arg(why.text()), true);
}

void Librarian::apply(const QList<Proposal> &proposals) {
    Library *library = m_backend->library();
    int applied = 0;
    QStringList problems;
    for (const Proposal &proposal : proposals) {
        QString error;
        if (Curation::apply(*library, proposal, &error)) {
            ++applied;
            if (proposal.coverUrl.isValid())
                m_backend->fetchCoverFor(proposal.bookId, proposal.coverUrl);
        } else {
            problems << error;
        }
    }
    m_entries.clear();
    emit proposalsChanged();
    m_backend->reload();
    m_backend->sayOutcome((QStringList{tr("%Ln changes made", "", applied)} + problems.mid(0, 3)).join("\n"), !problems.isEmpty());
    emit this->applied();
}
