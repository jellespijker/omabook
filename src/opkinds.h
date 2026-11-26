#pragma once

#include <QLatin1StringView>
#include <QString>
#include <span>

#include "spacereplay.h"

// The kinds of operation this Omabook knows (#118): what each is of (its
// entity) and, of a book, which of its fields. A newer Omabook may write a
// kind this one doesn't know (as one from before #68 knows no reading
// goal): this one keeps it in its log and sends it on, but never takes it
// in (KeptOps), until an Omabook that knows it opens the library. A plugin's data is all of one kind,
// whatever its plugin calls it (ADR 0002).
namespace OpKinds {

// A book's fields that travel by the log: its reading place, its details and
// what is said of its reading, each with the stamp of its time, and the links
// that line up its ids, its tags one by one (`tag:…`), and who built its
// knowledge graph (`graph`, GraphBuilt). Never where its
// files are here (`path`, `formats`): those are this computer's own. A
// book's removal travels as its tombstone.
[[nodiscard]] std::span<const QLatin1StringView> travellingBookFields();
[[nodiscard]] bool travelsOfABook(const QString &field);

[[nodiscard]] bool known(const SpaceOperation &op);

// For a test: while one lives, this Omabook (on this thread) knows no
// operation of `entity`, or with `field` only that field of it, as an
// Omabook from before them would.
class Forgetting {
public:
    explicit Forgetting(QString entity, QString field = {});
    ~Forgetting();
    Forgetting(const Forgetting &) = delete;
    Forgetting &operator=(const Forgetting &) = delete;
    Forgetting(Forgetting &&) = delete;
    Forgetting &operator=(Forgetting &&) = delete;

private:
    QString m_entity;
    QString m_field;
};

} // namespace OpKinds
