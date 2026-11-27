// The window's side of adding books (importer.h): what it says when they are
// in. Kept apart from the rest of Backend, as syncing's summary is.

#include "backend.h"

#include "importer.h"

void Backend::reportOpened(const ImportSummary &summary) {
    if (summary.needsReading())
        sayOutcome(importLines(summary, false).join("\n"), true);
}

QStringList Backend::importLines(const ImportSummary &summary, bool calibreLeftOut) {
    // A file that joined a book here without one was added, to the window.
    const int added = summary.added + summary.joined;
    const int duplicates = summary.duplicates;
    QStringList lines;
    if (added > 0)
        lines << tr("%Ln books added", "", added);
    if (duplicates > 0)
        lines << tr("%Ln were already in the library", "", duplicates);
    // What Calibre knew of the reading, which came along.
    if (summary.read > 0)
        lines << tr("%Ln are read, by Calibre's account", "", summary.read);
    if (summary.reading > 0)
        lines << tr("%Ln are being read", "", summary.reading);
    if (summary.updated > 0)
        lines << tr("%Ln already here were brought up to date", "", summary.updated);
    if (summary.skipped > 0)
        lines << tr("%Ln books of the Calibre library were not imported", "", summary.skipped);
    if (calibreLeftOut)
        lines << tr("The Calibre plugin is switched off, so the Calibre library was added as a plain folder of books.");
    const QStringList reasons = summary.reasons();
    if (added == 0 && duplicates == 0 && reasons.isEmpty())
        lines << tr("No books were found there.");
    // The first few reasons say enough here; a folder of odd files could give hundreds.
    lines << reasons.mid(0, 3);
    const int more = static_cast<int>(reasons.size()) - 3;
    if (more > 0)
        lines << (summary.leftOut.isEmpty() && summary.unreadable.isEmpty() ? tr("%Ln more could not be added", "", more)
                                            : tr("%Ln more are named in the log (omabook logs)", "", more));
    return lines;
}
