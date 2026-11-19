#pragma once

#include <QList>
#include <QString>
#include <optional>

#include "curation.h"
#include "library.h"

// The proposals `omabook collections sort` last listed, kept for picking
// from by their numbers without asking the AI again: in this computer's
// state folder, one file per library.
namespace KeptProposals {

[[nodiscard]] QString file(const Library &library);
void keep(const Library &library, const QList<Proposal> &proposals);
// Those of books still here; none when there was no sort.
[[nodiscard]] std::optional<QList<Proposal>> read(const Library &library);

} // namespace KeptProposals
