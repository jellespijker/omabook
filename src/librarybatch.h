#pragma once

#include <QString>
#include <chrono>
#include <memory>

#include "failure.h"

struct WriteLine;

// Why the library whose database is `file` wasn't written: it isn't there any
// more (#221), or another has been writing it for too long.
[[nodiscard]] Failure whyNotWritten(const QString &file);
// Makes a folder for a book's files while that library is there; false when it
// isn't, so nothing is made at the path it was at.
[[nodiscard]] bool makeBookFolder(const QString &database, const QString &folder);

// Whose turn it is to write a library, among the connections this program
// has open on it (#200): the window's, a command's, a sync's worker, the
// statistics counted beside the window, an AI job, a plugin. SQLite lets a
// writer that finds the library taken only try again now and then, and one
// writing batch after batch could take every moment between them, so the
// other was refused as "library busy". Here each waits in line, and has its
// turn once those before it are done. Another program (a command beside the
// window) is waited for by SQLite itself, as long.
//
// A batch takes the turn for as long as it lives (library.h), and a write
// outside a batch for that one write. The one whose turn it is takes it again
// at once for a batch inside a batch; through another connection on the same
// thread it is refused at once, as that one would only ever wait for itself.
class WriteTurn {
public:
    // How long a writer waits for the library at most, the turn and then
    // SQLite each: the longest a batch takes (setting up or bringing up to
    // date a large library, a sync bringing a shelf in), and then says so.
    static constexpr std::chrono::milliseconds longest = std::chrono::minutes(2);

    // Waits for the turn to write the database at `file` through
    // `connection`, for `wait` at most.
    WriteTurn(const QString &file, const QString &connection, std::chrono::milliseconds wait = longest);
    ~WriteTurn();
    WriteTurn(const WriteTurn &) = delete;
    WriteTurn &operator=(const WriteTurn &) = delete;
    WriteTurn(WriteTurn &&) = delete;
    WriteTurn &operator=(WriteTurn &&) = delete;

    // Whether it is this one's turn; when not, why is in the log.
    [[nodiscard]] bool had() const noexcept { return m_had; }

private:
    std::shared_ptr<WriteLine> m_line;
    bool m_had = false;
};
