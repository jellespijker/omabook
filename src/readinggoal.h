#pragma once

#include <QString>
#include <expected>

class Library;

// A goal for a year's reading (#68): so many books finished, or so many
// hours read. One's own, kept in the personal space's change log, so a
// goal set on one computer is the goal on every other one syncs with
// (ADR 0001): the latest set wins. It is never more than a number to
// look at; nothing reminds or warns.
namespace ReadingGoal {

enum class Kind { None, Books, Hours };

struct Goal {
    int year = 0;
    Kind kind = Kind::None;
    int target = 0;
    [[nodiscard]] bool set() const noexcept { return kind != Kind::None && target > 0; }
};

// "books", "hours" or "" for none, and back; nothing for a word it doesn't know.
[[nodiscard]] QString nameOf(Kind kind);
[[nodiscard]] std::expected<Kind, QString> kindNamed(const QString &name);

// The goal of a year, or one that isn't set.
[[nodiscard]] Goal of(const Library &library, int year);
// Sets it (a target of 0, or Kind::None, takes it away); why not, when it can't be.
[[nodiscard]] std::expected<void, QString> set(const Library &library, const Goal &goal);

} // namespace ReadingGoal
