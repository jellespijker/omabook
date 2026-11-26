#pragma once

#include <QDateTime>
#include <QString>
#include <QtTypes>
#include <chrono>
#include <cstdint>
#include <optional>

#include "bookplace.h"

class Library;

// Who built a book's knowledge graph, and when (#219): the book's field
// "graph" in the change log, which travels with its other fields (ADR 0001),
// so a computer learns from a sync that one of the person's hubs built the
// graph of a book (#192) or is building it, and leaves it to the hub rather
// than spend its own AI on the same book again. The last word on it wins, as
// of any field: the hub says it began, then that it built it or stopped.
//
// {"state": "building" | "built" | "stopped", "by": "<its name>", "hub": true,
//  "at": <seconds since 1970>, "to": {"section", "fraction"}}, `to` of a
// graph built: how far, which a computer then takes for its own (SyncTakeGraph).
namespace GraphBuilt {

enum class State : std::uint8_t { Building, Built, Stopped };

struct Mark {
    State state = State::Built;
    QString by; // the name of what built it, a hub as it calls itself
    bool hub = false;
    QDateTime at; // when it began (building), or ended
    std::optional<BookPlace> to;

    [[nodiscard]] QString written() const;
    // The mark a value says; nothing for one that says none.
    [[nodiscard]] static std::optional<Mark> read(const QString &value);
};

// How long a hub that began a book's graph is waited for: then a computer
// builds it as it is read, as it would without a hub.
inline constexpr std::chrono::hours patience{12};

// The mark of a book, as the log has it now.
[[nodiscard]] std::optional<Mark> of(const Library &library, qint64 book);
void mark(const Library &library, qint64 book, const Mark &mark);

// Whether a computer leaves the graph of a book to a hub `now`: one built it,
// or began to less than `patience` ago. Asked by the person, it builds anyway.
[[nodiscard]] bool leftToHub(const std::optional<Mark> &mark, const QDateTime &now);

} // namespace GraphBuilt
