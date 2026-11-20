#pragma once

#include <QStringList>

// The `chronology` command (#45): the events of the books read in the order
// they happen in their world, cut where the reader is, and times, orders
// and timelines put in by hand.
int runChronologyCli(const QStringList &arguments);
// `omabook graph ...`, where `omabook graph chronology ...` is the same command
// as `omabook chronology ...`, as the issue first named it.
int runGraphCliWithChronology(const QStringList &arguments);
