#pragma once

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
};

[[nodiscard]] QList<Held> held(const Library &library);

// How a member of a space goes by there, where it is known; empty where not.
[[nodiscard]] QString memberName(const Library &library, const QString &space, const QString &person);

} // namespace HeldSpaces
