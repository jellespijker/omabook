#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <functional>

class Library;

// The one-off repairs a library is given as it is brought up to date (ADR
// 0001, Migration): each is a format of its own, whose step only marks it due
// (steps(), after LibrarySchema's tables), and is done once the change log is
// up to date (run()), in the transaction of the upgrade, after its copy.
namespace LibraryRepairs {

// The steps of formats 43 on, each marking its repair due (and 47's, a
// column a sync needs, #215).
[[nodiscard]] const QList<QStringList> &steps();
// Each repair that is due, done and said (`say` may be empty).
void run(Library &library, const std::function<void(const QString &)> &say);

} // namespace LibraryRepairs
