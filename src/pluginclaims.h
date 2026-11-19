#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <map>

#include "library.h"

// Which plugin made a book is said by its mark, `generated`, and the mark is
// what lets that plugin rewrite, fork and remove the book. A plugin sets it
// when it makes the book (omabook.library.create); a sync never carries it
// (synctake.cpp), so a book that came from another computer arrives without
// it. There its plugin claims it back: a book is a plugin's own when every
// one of its files is in a format that plugin registers
// (omabook.format.register) and Omabook doesn't take itself, and when no
// other plugin registers that format too. Never otherwise: a book by a
// person (an EPUB, a PDF) is no plugin's, whatever a plugin registers.
namespace PluginClaims {

// The extensions of the formats each plugin registers, by plugin.
using Registered = std::map<QString, QStringList>;

// The plugin whose own a book with files in `formats` is, or nothing.
[[nodiscard]] QString owner(const QStringList &formats, const Registered &plugins);

// Marks each of `books` that has no maker yet as its owner's. Answers the ids
// it marked.
QList<qint64> claim(Library &library, const QList<Book> &books, const Registered &plugins);

} // namespace PluginClaims
