#pragma once

#include <QLockFile>
#include <QString>
#include <QStringList>
#include <QtTypes>
#include <chrono>
#include <expected>
#include <memory>
#include <vector>

#include "failure.h"

// Makes adding a book one step among all who add to a library (#228): the
// window, commands, the MCP server and watched folders each look whether a
// file is there and then bring it in, and two at once would both find it
// missing. Whoever holds a file (by its hash) looks and brings it in alone;
// others that name the same file wait, then find it. Other files go on
// meanwhile. A lock file in the library's folder says who holds it, so it
// counts between programs and between threads, and one left by a program that
// died is taken over. A thread that holds one takes it again at once.
//
// So that two never wait for each other, files are taken first, all of them
// together, and a book after them.
class AddingHold {
public:
    // How long one waits at most: longer than copying a very large book takes.
    static constexpr std::chrono::milliseconds longest = std::chrono::minutes(10);

    // The files of these hashes, in the library in `root`; an empty hash (a
    // file that can't be read) names none.
    [[nodiscard]] static std::expected<AddingHold, Failure> files(const QString &root, QStringList hashes,
                                                                  std::chrono::milliseconds wait = longest);
    // A book while its first file is brought in: held by whoever makes it, or
    // gives one to a book without any, so only one file at a time does that.
    [[nodiscard]] static std::expected<AddingHold, Failure> book(const QString &root, qint64 id, std::chrono::milliseconds wait = longest);

private:
    [[nodiscard]] std::expected<void, Failure> take(const QString &file, std::chrono::milliseconds wait);

    std::vector<std::shared_ptr<QLockFile>> m_locks;
};
