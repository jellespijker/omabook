#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QtTypes>
#include <expected>

#include "failure.h"

class KosyncClient;
class Library;

// Omabook as a client of a KOReader sync server (#71): each book's reading
// place both ways, matched by the hash KOReader names each of its files by.
// The newer place wins: a device's is taken here when it is newer than the
// book's place here, and the place here is sent when it is the newer one.
// What Omabook itself sent is never taken back as a device's. It waits for
// every answer, so it is not for the window's thread.
namespace KoreaderSync {

struct Summary {
    int taken = 0; // books whose place here a device moved
    int sent = 0;  // books whose place here went to the server
    int books = 0; // books looked at

    // {"taken", "sent", "books"}
    [[nodiscard]] QJsonObject toJson() const;
};

// Syncs these books, or every book with a file when none is named. The
// first failure to reach the server, or to be let in, ends it.
[[nodiscard]] std::expected<Summary, Failure> run(Library &library, KosyncClient &client, const QList<qint64> &only = {});

} // namespace KoreaderSync
