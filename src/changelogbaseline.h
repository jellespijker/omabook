#pragma once

#include <QHash>
#include <QString>
#include <QtTypes>

#include "changelog.h"

class Library;

// The change log's baseline (ADR 0001, Migration 2), written as a snapshot
// (#54). While one is being written on a thread, every operation ChangeLog
// appends there is held in the snapshot (Compaction::holdInSnapshot) instead
// of `ops`: the same operations, with the same values and times, numbered
// as they would have been, but unsigned and without a statement each for
// what stays the same throughout (this computer, its spaces, the last number
// of each). finish() moves the marks on to them and records them as seen,
// so the log carries on after the snapshot.
class ChangeLogBaseline {
public:
    explicit ChangeLogBaseline(const Library &library);
    ~ChangeLogBaseline();
    ChangeLogBaseline(const ChangeLogBaseline &) = delete;
    ChangeLogBaseline &operator=(const ChangeLogBaseline &) = delete;
    ChangeLogBaseline(ChangeLogBaseline &&) = delete;
    ChangeLogBaseline &operator=(ChangeLogBaseline &&) = delete;

    // The baseline being written on this thread, if one is.
    [[nodiscard]] static ChangeLogBaseline *writing() noexcept;

    // Stamps and numbers `entry` as ChangeLog::append would, and holds it in the snapshot.
    void hold(const ChangeLog::Entry &entry);
    // Done: the marks moved on to what the snapshot holds, and that seen.
    void finish();

private:
    [[nodiscard]] qint64 idOf(const QString &name);

    const Library &m_library;
    ChangeLogBaseline *m_outer;
    QString m_personal;
    qint64 m_device = 0;
    qint64 m_clock = 0;           // the latest stamp observed, of which observing again changes nothing
    QHash<QString, qint64> m_ids; // names in log_ids
    QHash<qint64, qint64> m_last; // per space, the last number this device gave
};
