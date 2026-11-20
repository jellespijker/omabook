#include "booksides.h"

#include <QDateTime>
#include <QFileInfo>

#include "settings.h"

BookSides::BookSides(const QList<Collection> &collections, const QList<Book> &books) {
    if (collections.isEmpty())
        return;
    // The tree is read from the top, so each collection's top is the last
    // one at the top before it.
    struct Held {
        Collection::Part part;
        qint64 top = 0;
    };
    QList<Held> held;
    qint64 top = 0;
    for (const Collection &collection : collections) {
        if (collection.depth == 0)
            top = collection.id;
        if (!collection.removed)
            held.append({{collection.rule, collection.included, collection.excluded}, top});
    }
    for (const Book &book : books) {
        Side side;
        for (const Held &one : std::as_const(held))
            if (!side.contains(one.top) && one.part.holds(book))
                side.insert(one.top);
        if (side.isEmpty())
            side.insert(none);
        m_sides.insert(book.id, side);
    }
}

BookSides BookSides::of(const Library &library) {
    // What the user chose, read again only when config.json changed.
    struct Choice {
        QString file;
        QDateTime modified;
        qint64 size = -1;
        bool across = false;
    };
    thread_local Choice choice;
    const QFileInfo config(Settings::defaultFile());
    if (config.absoluteFilePath() != choice.file || config.lastModified() != choice.modified || config.size() != choice.size)
        choice = {config.absoluteFilePath(), config.lastModified(), config.size(), Settings().graphOneTenant()};
    if (choice.across)
        return {};

    // Any change to the books or the collections moves this counter.
    struct Kept {
        QString database;
        qint64 books = -1;
        BookSides sides;
    };
    thread_local Kept kept;
    const QString database = library.databasePath();
    const qint64 books = library.changes().books;
    if (kept.database != database || kept.books != books) {
        const QList<Collection> collections = library.collections();
        kept = {database, books, collections.isEmpty() ? BookSides() : BookSides(collections, library.books())};
    }
    return kept.sides;
}
