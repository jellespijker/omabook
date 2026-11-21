#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include "spacereplay.h"
#include "subscribedspace.h"

class Library;

// The spaces this library holds of other people's (ADR 0001, *Spaces*): each
// published space it subscribes to and each shared space it is a member of,
// with what it accepts of them and who it is there, which every layer
// beside one's own (of plugins' worlds, WorldLayers; of annotations,
// AnnotationLayers) is read from.
namespace HeldSpaces {

struct Held {
    SubscribedSpace::Subscription space;
    QString kind;      // "published" or "shared"
    QString role;      // this library's in it: "reader" or "writer"
    QString me;        // whose writes are this library's own, which are no layer here
    QStringList parts; // of a shared space, the parts it holds now (SharedSpace::State::parts)
    QList<SpaceOperation> ops;
    // How its members go by there, as each chose (MemberNames; of a
    // published space its owner, as they published it), made plain.
    QHash<QString, QString> names;

    // How `person` goes by in it; empty where they chose no name.
    [[nodiscard]] QString nameOf(const QString &person) const { return names.value(person); }
};

[[nodiscard]] QList<Held> held(const Library &library);

} // namespace HeldSpaces
