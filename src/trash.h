#pragma once

#include <QString>
#include <expected>

#include "failure.h"

// What is removed goes to the trash, where a mistake can be undone, and
// nowhere else: where no trash folder can be made (a disk or share whose top
// folder can't be written, say), it stays, since nobody was asked whether it
// may go for good (#285).
namespace Trash {

// Moves a file or folder to the trash, making the home folder's trash when
// there is none yet. False when it can't go there: it is then where it was.
// `where`, when asked for, is where it is in the trash.
[[nodiscard]] bool take(const QString &path, QString *where = nullptr);
// Takes a folder inside `root` away: nothing to do when it isn't there, or is
// `root` itself or outside it; removed when nothing is in it; else to the trash.
[[nodiscard]] std::expected<void, Failure> folder(const QString &root, const QString &folder);

} // namespace Trash
