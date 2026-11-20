#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QtTypes>
#include <optional>

class Library;

// Owned spaces and content-derived global ids (ADR 0001).
//
// Each library has one device id of its own and a personal space, plus a
// space for each story world ("world:…"). Every row that travels is named by
// a global id in `global_ids`, derived from what the thing is so two
// computers that make the id of the same thing independently make the same
// one.
class Spaces {
public:
    // This library's device id (one per library, created on first ask).
    [[nodiscard]] static QString deviceId(const Library &library);

    // Ensures this library's personal space exists and answers its id.
    static void ensurePersonalSpace(const Library &library);
    [[nodiscard]] static QString personalSpace(const Library &library);
    // Pairs this library with another computer's: two of one's own computers
    // that sync already trust each other (one reaches the other over ssh),
    // so the first replication between them makes their personal spaces one.
    // Each knows the other's, and both keep the one with the smaller id
    // (personalSpace), the two devices members of both. The operations
    // already written in the other space stay where they are (the log is
    // append-only) and travel and merge with the rest.
    static void pair(const Library &library, const QString &theirSpace, const QString &theirDevice, const QString &theirName = {});
    // Whether `device` is one of one's own computers: a member of the personal space.
    [[nodiscard]] static bool isOwnComputer(const Library &library, const QString &device);
    // One's own computers, paired into the personal space: each device's id
    // and the name it goes by (as it said when paired; empty when unknown).
    struct Computer {
        QString device;
        QString name;
    };
    [[nodiscard]] static QList<Computer> computers(const Library &library);
    // The space a graph scope belongs to: the personal space for "", and a
    // story-world space for "world:…".
    static void ensureSpaceForScope(const Library &library, const QString &scope);
    [[nodiscard]] static QString spaceForScope(const Library &library, const QString &scope);

    // Every space recorded here, as `spaces` keeps it, by kind and name.
    struct Recorded {
        QString id;
        QString kind;
        QString name;
        QString owner;
    };
    [[nodiscard]] static QList<Recorded> recorded(const Library &library);

    // Records a published space (ADR 0001, *Spaces*): written only by its
    // owner, the person `owner` (Person::id), who goes by `ownerName` in it.
    static void ensurePublishedSpace(const Library &library, const QString &space, const QString &name, const QString &owner,
                                     const QString &ownerName);

    // The library's persistent hybrid logical clock in `library_state`.
    [[nodiscard]] static qint64 lastHlc(const Library &library);
    [[nodiscard]] static qint64 tickHlc(const Library &library, std::optional<qint64> wallSeconds = std::nullopt);
    static void observeHlc(const Library &library, qint64 remote);
    static void setClockOffset(const Library &library, qint64 offsetSeconds);
    [[nodiscard]] static qint64 clockOffset(const Library &library);
    [[nodiscard]] static qint64 wallSeconds(const Library &library);

    // Pure content-derived global-id rules.
    [[nodiscard]] static QString bookGlobalId(const QStringList &fileHashes, const QStringList &entryKeys);
    [[nodiscard]] static QString collectionGlobalId(const QString &path);
    [[nodiscard]] static QString nodeGlobalId(const QString &scope, const QString &kind, const QString &name);
    [[nodiscard]] static QString edgeGlobalId(const QString &sourceGlobalId, const QString &targetGlobalId, const QString &kind);
    [[nodiscard]] static QString factGlobalId(const QString &nodeGlobalId, const QString &kind, const QString &value,
                                              const QString &bookGlobalId = {});
    [[nodiscard]] static QString removalGlobalId(const QString &removalKey);
    // A bookmark or highlight (#66), by its book and what it is where, so the
    // same one taken in on two computers (from Calibre, or a copy of the
    // library) is one; renamed after, it keeps the id it was given.
    [[nodiscard]] static QString annotationGlobalId(const QString &bookGlobalId, const QString &kind, int section, double fraction,
                                                    const QString &text, const QString &note);

    // Looks up or records global ids in `global_ids`.
    [[nodiscard]] static QString globalId(const Library &library, const QString &entity, qint64 localId);
    [[nodiscard]] static qint64 localId(const Library &library, const QString &entity, const QString &globalId);
    static void setGlobalId(const Library &library, const QString &entity, qint64 localId, const QString &globalId);

    // Ensures a row has its global id in `global_ids` and answers it.
    static void ensureBookId(const Library &library, qint64 localId);
    static void ensureCollectionId(const Library &library, qint64 localId);
    static void ensureNodeId(const Library &library, qint64 localId);
    static void ensureEdgeId(const Library &library, qint64 localId);
    static void ensureFactId(const Library &library, qint64 localId);
    static void ensureAnnotationId(const Library &library, qint64 localId);
    [[nodiscard]] static QString bookId(const Library &library, qint64 localId);
    [[nodiscard]] static QString collectionId(const Library &library, qint64 localId);
    [[nodiscard]] static QString nodeId(const Library &library, qint64 localId);
    [[nodiscard]] static QString edgeId(const Library &library, qint64 localId);
    [[nodiscard]] static QString factId(const Library &library, qint64 localId);
    [[nodiscard]] static QString annotationId(const Library &library, qint64 localId);

    // Gives every existing row in the library its global id.
    static void assignAll(const Library &library);
};
