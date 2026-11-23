#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtTypes>

#include "library.h"

// What a sync is to change of the books on one side, as its dry run decides
// it, for the sync guard to check before anything is written (#187): each
// book it changes, as it is and as it would be; the books it removes; the
// reading places it moves; and the bookmarks and highlights it would give
// another book. Each side of a sync makes its own (SyncTake records into it),
// and the side asked hands its own back with its answer.
struct SyncPlan {
    // A book as the guard looks at it: its details, the identifiers it has,
    // what is said of its reading, whether it has a cover, and "names" (its
    // files' hashes, else its entry keys: what a removal names it by).
    using State = QJsonObject;
    // Every book of a side, by the global id it has there.
    using States = QHash<QString, State>;

    struct Change {
        State before;
        State after;
        bool weak = false; // paired on less than its global id (BookLinks::weaklyPaired, #158)
    };
    // When the place a book is read to was read there, and when the one it would take was.
    struct Place {
        QString title;
        qint64 before = 0;
        qint64 after = 0;
    };

    bool said = false;            // whether the side made one: an Omabook from before the guard makes none
    QHash<QString, Change> books; // by global id
    QHash<QString, State> removed;
    QList<Place> places;
    QStringList annotationsMoved; // the titles of the books a bookmark or highlight would move to

    [[nodiscard]] static State stateOf(const Library &library, const Book &book);
    [[nodiscard]] static States states(const Library &library);
    [[nodiscard]] static QJsonObject toJson(const States &states);
    [[nodiscard]] static States statesFrom(const QJsonObject &object);

    // As a take decides it, of `book` as it is here: its details become
    // `meta`'s (with a cover when it takes one), what is said of its reading
    // these, it gains an identifier, or it goes.
    void details(const Library &library, const Book &book, const BookMeta &meta, bool cover, bool weak);
    void reading(const Library &library, const Book &book, const Book &filled);
    void identifier(const Library &library, const Book &book, const QString &scheme, const QString &value);
    void removal(const Library &library, const Book &book);

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static SyncPlan fromJson(const QJsonObject &object);

private:
    Change &of(const Library &library, const Book &book);
};
