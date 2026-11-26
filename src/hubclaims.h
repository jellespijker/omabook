#pragma once

#include <QDateTime>
#include <QString>
#include <chrono>
#include <expected>
#include <optional>

#include "failure.h"

class Library;

// Which of the person's hubs builds a book's graph (#196, ADR 0011): hubs are
// peers, so two of them that keep the same library would otherwise both take
// up a new book. The hub that takes one up claims it first, in the library's
// change log (PluginData, under "omabook-hub"), so the claim travels with the
// library by the normal syncs; a hub that finds another's claim, made within
// `lease`, leaves the book to it, and gets the graph by the sync once it is
// built. Two hubs that claimed it before either knew of the other end with the
// same claim, the later, wherever both claims meet (the log's latest wins):
// the one whose claim was overtaken keeps nothing of its work.
namespace HubClaims {

// How long a claim holds: one not renewed by then is taken over, so a hub that went away holds up nothing for good.
inline constexpr std::chrono::hours lease{24};

struct Claim {
    QString by; // the device of the hub's library that claimed it
    QDateTime at;
};

// The claim on the graph of the book of global id `book`, if there is one.
[[nodiscard]] std::optional<Claim> of(const Library &library, const QString &book);
// Whether another hub than this library's holds the claim at `now`.
[[nodiscard]] bool elsewhere(const Library &library, const QString &book, const QDateTime &now);
// This library's hub claims it at `now`.
[[nodiscard]] std::expected<void, Failure> claim(const Library &library, const QString &book, const QDateTime &now);

} // namespace HubClaims
