#include "keptproposals.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

QString KeptProposals::file(const Library &library) {
    const QString state =
        qEnvironmentVariableIsSet("XDG_STATE_HOME") ? qEnvironmentVariable("XDG_STATE_HOME") : QDir::homePath() + "/.local/state";
    return state + "/omabook/collections-sort-"
           + QString::fromLatin1(QCryptographicHash::hash(library.root().toUtf8(), QCryptographicHash::Sha1).toHex().left(16)) + ".json";
}

void KeptProposals::keep(const Library &library, const QList<Proposal> &proposals) {
    QJsonArray array;
    for (const Proposal &proposal : proposals)
        array << QJsonObject{{"kind", proposal.kind},
                             {"id", proposal.bookId},
                             {"collection_id", proposal.collectionId},
                             {"collection", proposal.collection},
                             {"why", proposal.why}};
    QDir().mkpath(QFileInfo(file(library)).absolutePath());
    QSaveFile saved(file(library));
    if (saved.open(QIODevice::WriteOnly) && saved.write(QJsonDocument(array).toJson(QJsonDocument::Compact)) > 0)
        saved.commit();
}

std::optional<QList<Proposal>> KeptProposals::read(const Library &library) {
    QFile kept(file(library));
    if (!kept.open(QIODevice::ReadOnly))
        return std::nullopt;
    QList<Proposal> proposals;
    for (const QJsonValue value : QJsonDocument::fromJson(kept.readAll()).array()) {
        const QJsonObject object = value.toObject();
        const auto book = library.book(object.value("id").toInteger());
        if (!book)
            continue;
        Proposal proposal;
        proposal.kind = object.value("kind").toString();
        proposal.bookId = book->id;
        proposal.meta = book->meta;
        proposal.collectionId = object.value("collection_id").toInteger();
        proposal.collection = object.value("collection").toString();
        proposal.why = object.value("why").toString();
        proposals << proposal;
    }
    return proposals;
}
