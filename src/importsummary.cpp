// What an import says it did (ImportSummary, importer.h), in the shapes the
// command line, the MCP tools and the window show it in.

#include "importer.h"

#include "log.h"

using namespace Qt::StringLiterals;

QJsonObject ImportSummary::Item::toJson() const {
    return {{"file", file}, {"id", id}, {"duplicate", duplicate}, {"joined", joined}, {"error", error}};
}

void ImportSummary::skip(const QString &file, const Failure &why, const QList<Failure> &files) {
    leftOut.insert(skipped++, why);
    QStringList said{why.text()};
    for (const Failure &one : files)
        said << one.text();
    notImported << said.join(u' ');
    unusable.append({.file = file, .error = notImported.constLast()});
}

bool ImportSummary::foundNothing() const { return items.isEmpty() && problems.isEmpty() && leftOut.isEmpty(); }

bool ImportSummary::failed() const { return !problems.isEmpty() || skipped > 0; }

bool ImportSummary::needsReading() const { return failed() || !leftOut.isEmpty() || !unreadable.isEmpty(); }

QStringList ImportSummary::reasons() const {
    QStringList reasons;
    for (const Failure &left : leftOut)
        reasons << left.text();
    reasons += problems;
    for (const Failure &one : unreadable)
        reasons << one.text();
    return reasons;
}

QJsonArray ImportSummary::itemsJson() const {
    QJsonArray array;
    for (const Item &item : items + unusable)
        array << item.toJson();
    return array;
}

QJsonObject ImportSummary::calibreJson() const {
    QJsonArray left;
    for (const Failure &one : leftOut)
        left << one.text();
    return {{"libraries", QJsonArray::fromStringList(calibreLibraries)},
            {"added", added},
            {"joined", joined},
            {"already_here", duplicates},
            {"updated", updated},
            {"read", read},
            {"reading", reading},
            {"skipped", skipped},
            {"not_imported", QJsonArray::fromStringList(notImported)},
            {"left_out", left},
            {"problems", QJsonArray::fromStringList(problems)}};
}

QString ImportSummary::calibreLine() const {
    const QString line = u"%1 added, %2 joined to books here without a file, %3 already here (%4 brought up to date); "
                         "%5 read, %6 being read"_s.arg(added)
                             .arg(joined)
                             .arg(duplicates)
                             .arg(updated)
                             .arg(read)
                             .arg(reading);
    return skipped > 0 ? line + u"; %1 not imported"_s.arg(skipped) : line;
}

void ImportSummary::keepInLog() const {
    for (const Failure &left : leftOut + unreadable)
        Log::keep(lcLibrary(), left.english());
    for (const QString &problem : problems)
        Log::keep(lcLibrary(), problem);
}
