#include "changelogbaseline.h"

#include <QSqlQuery>
#include <QVariant>
#include <utility>

#include "compaction.h"
#include "hlc.h"
#include "library.h"
#include "spaces.h"

namespace {

ChangeLogBaseline *&current() noexcept {
    thread_local ChangeLogBaseline *baseline = nullptr;
    return baseline;
}

} // namespace

ChangeLogBaseline::ChangeLogBaseline(const Library &library)
    : m_library(library), m_outer(std::exchange(current(), this)), m_personal(Spaces::personalSpace(library)),
      m_device(ChangeLog::internId(library, Spaces::deviceId(library))), m_clock(Spaces::lastHlc(library)) {}

ChangeLogBaseline::~ChangeLogBaseline() { current() = m_outer; }

ChangeLogBaseline *ChangeLogBaseline::writing() noexcept { return current(); }

qint64 ChangeLogBaseline::idOf(const QString &name) {
    if (const auto known = m_ids.constFind(name); known != m_ids.cend())
        return *known;
    const qint64 id = ChangeLog::internId(m_library, name);
    m_ids.insert(name, id);
    return id;
}

void ChangeLogBaseline::hold(const ChangeLog::Entry &entry) {
    const qint64 space = idOf(entry.space.isEmpty() ? m_personal : entry.space);
    const qint64 stamp = entry.hlc ? Hlc::normalize(*entry.hlc) : Spaces::tickHlc(m_library);
    // Observing a time is keeping the later of it and the clock, so only one
    // later than any so far changes the clock.
    if (entry.hlc && *entry.hlc > 0 && stamp > m_clock) {
        Spaces::observeHlc(m_library, stamp);
        m_clock = stamp;
    }
    auto last = m_last.find(space);
    if (last == m_last.end()) {
        // After whatever this device has of the space already, as ChangeLog::append numbers.
        QSqlQuery held = ChangeLog::query(m_library,
                                          "SELECT MAX(COALESCE((SELECT MAX(seq) FROM ops WHERE space = ? AND device = ?), 0), "
                                          "COALESCE((SELECT MAX(seq) FROM snapshot_ops WHERE space = ? AND device = ?), 0), "
                                          "COALESCE((SELECT seq FROM snapshot_marks WHERE space = ? AND device = ?), 0))",
                                          {space, m_device, space, m_device, space, m_device});
        last = m_last.insert(space, held.next() ? held.value(0).toLongLong() : 0);
    }
    Compaction::holdInSnapshot(m_library, {.space = space,
                                           .device = m_device,
                                           .seq = ++*last,
                                           .hlc = stamp,
                                           .entity = entry.entity,
                                           .id = idOf(entry.id),
                                           .field = entry.field,
                                           .value = entry.value});
}

void ChangeLogBaseline::finish() {
    Compaction::markHeld(m_library);
    ChangeLog::exec(m_library,
                    "INSERT INTO seen (space, device, seq, hlc) SELECT space, device, MAX(seq), MAX(hlc) FROM snapshot_ops "
                    "WHERE device = ? GROUP BY space ON CONFLICT(space, device) DO UPDATE SET seq = MAX(seq, excluded.seq), "
                    "hlc = MAX(hlc, excluded.hlc)",
                    {m_device});
}
