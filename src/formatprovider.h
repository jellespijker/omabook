#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>
#include <optional>

#include "bookmeta.h"
#include "booksource.h"
#include "document.h"

// How a format is added to Omabook without touching the reader.
//
// A format that flows (chapters of text) never implements Document. It is a
// provider: something that looks at a book's files and describes the book as
// plain data, chapters of HTML with a title each. Omabook turns that
// description into a Document itself: it cleans the HTML, sets it in the
// reader's font, finds the pictures the chapters refer to among the book's
// files, and makes links between chapters work. So the reader, search, the
// command line, the MCP server and the AI all work with a new format the
// moment its provider is registered.
//
// Providers are registered at run time (Formats::addProvider), by Omabook
// itself for the formats it ships this way (Markdown books, markdownbook.cpp)
// and by plugins for theirs. A provider's functions may be called from any
// thread, one call at a time per provider.

// A book as a provider describes it.
struct BookDescription {
    struct Chapter {
        QString title;  // for the contents; empty to leave the chapter out of them
        QString html;   // any HTML; Omabook keeps what the reader can show
        // The file the chapter came from, when there is one: pictures and
        // links in the chapter are found relative to it, and a link to
        // another chapter's file leads to that chapter.
        QString file;
        int level = 0;  // how deep it sits in the contents, 0 for the top
    };
    BookMeta meta;
    QList<Chapter> chapters;
    QString cover;      // the name of the cover's file in the book, if any
};

struct FormatProvider {
    // Unique and stable: "markdown", or "plugin:<name>" for a plugin's.
    QString id;
    // What the format is called: "Markdown book".
    QString name;
    // The extensions of files in this format, lower case, without the dot.
    // The first is the one a folder or a plain archive of this format is
    // packed as when it joins a library, which holds one file per book.
    QStringList extensions;
    // Whether a folder, or an archive with no extension of its own (a .zip),
    // is a book in this format. Left empty, only the extensions count.
    std::function<bool(const BookSource &)> claims;
    // The book in these files; nothing, and why in English, when it can't be read.
    std::function<std::optional<BookDescription>(const BookSource &, QString *error)> describe;
};

namespace Formats {

// Adds a format, replacing one with the same id. Its extensions join the
// ones Omabook takes into a library; an extension Omabook reads itself stays
// with Omabook.
void addProvider(const FormatProvider &provider);
void removeProvider(const QString &id);
QList<FormatProvider> providers();
// Something to call before the providers are looked through, so that
// whoever keeps them can bring them up to date first: a command has no
// window to load plugins for it, and loads them only once a format Omabook
// doesn't read itself is asked about. Empty to have nothing called.
void setProviderLoader(std::function<void()> loader);
// The provider for a file by its extension, or for a folder or plain archive
// by what is in it; nothing when no provider wants it.
std::optional<FormatProvider> providerFor(const QString &path);

// The Document a description makes; `source` is where its pictures are read from.
std::unique_ptr<Document> fromDescription(const BookDescription &description, const BookSource &source);

} // namespace Formats

// The provider for Markdown books: a folder or zip of Markdown files.
FormatProvider markdownBookProvider();
