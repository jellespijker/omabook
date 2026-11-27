// Whose a hub is once its owner's id has changed (#283, ADR 0012): pairing
// gives a person's computers one id, and the hub takes the new one only on
// the word of a key it already knows as its owner's, said for that computer's
// key alone. Nobody else is let in: not another person, not one who copies
// that word, and not a computer that only signs for itself.

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include <optional>

#include "fixtures.h"
#include "hubcomputers.h"
#include "hubowner.h"
#include "hubsession.h"
#include "person.h"
#include "sshkey.h"

using Fixtures::whyNot;
using namespace Qt::StringLiterals;

namespace {

// A hub in a folder of its own, a computer's key, and three persons: the one
// the hub was first linked by, the one their computers go by since, and a stranger.
struct Hub {
    QTemporaryDir dir;
    HubComputers computers{dir.filePath(u"state"_s), dir.filePath(u"home/.ssh/authorized_keys"_s), {u"/usr/bin/omabook"_s}};
    std::expected<Person, QString> first = Person::generate();
    std::expected<Person, QString> now = Person::generate();
    std::expected<Person, QString> stranger = Person::generate();

    // A computer's key of its own, as the hub writes it.
    [[nodiscard]] QString key(const QString &name) const {
        const auto made = SshKey::generate(dir.filePath(name), name);
        return made ? SshKey::line(*made) : QString();
    }

    [[nodiscard]] static HubComputers::Asked asking(const QString &name, const QString &key, const Person &person,
                                                    const QString &former = {}) {
        return {.name = name,
                .key = key,
                .identity = person.id(),
                .signature = HubOwner::signature(person, HubComputers::linkMessage(name, key)),
                .former = former};
    }
};

[[nodiscard]] QJsonObject helloSaying(const Person &now, const Person &former, const QString &key) {
    QJsonObject hello = HubSession::hello();
    hello.insert("identity"_L1, now.id());
    hello.insert("signature"_L1, HubOwner::signature(now, HubOwner::message(now.id(), key)));
    hello.insert("former"_L1, HubOwner::vouch(former, now.id(), key));
    return hello;
}

[[nodiscard]] bool greeted(HubComputers &computers, const QString &fingerprint, const QJsonObject &hello) {
    HubSession session(computers, fingerprint, [](const QByteArray &) { return QByteArray(); });
    return QJsonDocument::fromJson(session.answer(QJsonDocument(hello).toJson(QJsonDocument::Compact))).object().value("ok").toBool();
}

} // namespace

class HubOwnerTests : public QObject {
    Q_OBJECT

private slots:
    void aLinkUnderANewIdNeedsTheWordOfAnOwnersKey() {
        Hub hub;
        QVERIFY2(hub.first && hub.now && hub.stranger, "made");
        const QString laptop = hub.key(u"laptop"_s);
        const QString desktop = hub.key(u"desktop"_s);
        QVERIFY(hub.computers.link(Hub::asking(u"laptop"_s, laptop, *hub.first)));
        const auto refused = [&hub, &desktop](const Person &person, const QString &former) {
            const auto linked = hub.computers.link(Hub::asking(u"desktop"_s, desktop, person, former));
            return !linked && linked.error().code() == Failure::Code::HubNotOwner;
        };
        // No word; a stranger's own; the owner's id named over a stranger's signature; a word for another person;
        // and one said for another computer's key.
        QVERIFY(refused(*hub.now, {}));
        QVERIFY(refused(*hub.stranger, HubOwner::vouch(*hub.stranger, hub.stranger->id(), desktop)));
        const QString forged = HubOwner::vouch(*hub.stranger, hub.stranger->id(), desktop);
        QVERIFY(refused(*hub.stranger, hub.first->id() + forged.mid(forged.indexOf(u'.'))));
        QVERIFY(refused(*hub.stranger, HubOwner::vouch(*hub.first, hub.now->id(), desktop)));
        QVERIFY(refused(*hub.now, HubOwner::vouch(*hub.first, hub.now->id(), laptop)));
        QVERIFY(refused(*hub.now, u"no vouch at all"_s));
        QCOMPARE(hub.computers.computers().size(), 1);
        QVERIFY(!hub.computers.isOwner(hub.now->id()) && !hub.computers.isOwner(hub.stranger->id()));
        QVERIFY(!hub.computers.isOwner(QString()));
        // The owner's word for this computer's key, among others that say nothing here.
        const QString words = forged + u'.' + HubOwner::vouch(*hub.first, hub.now->id(), desktop);
        const auto linked = hub.computers.link(Hub::asking(u"desktop"_s, desktop, *hub.now, words));
        QVERIFY2(linked, qPrintable(whyNot(linked)));
        QCOMPARE(hub.computers.owner(), hub.first->id());
        QVERIFY(hub.computers.isOwner(hub.first->id()) && hub.computers.isOwner(hub.now->id()));
        // Known from now on: no word needed, and its own word goes on from it.
        const QString third = hub.key(u"third"_s);
        QVERIFY(hub.computers.link(Hub::asking(u"third"_s, third, *hub.now)));
        const QString fourth = hub.key(u"fourth"_s);
        QVERIFY(hub.computers.link(Hub::asking(u"fourth"_s, fourth, *hub.stranger, HubOwner::vouch(*hub.now, hub.stranger->id(), fourth))));
    }

    void aSessionsHelloSaysWhoTheOwnerIsNow() {
        Hub hub;
        QVERIFY2(hub.first && hub.now && hub.stranger, "made");
        const QString laptop = hub.key(u"laptop"_s);
        const auto linked = hub.computers.link(Hub::asking(u"laptop"_s, laptop, *hub.first));
        QVERIFY2(linked, qPrintable(whyNot(linked)));
        const QString &fingerprint = linked->fingerprint;
        // The session goes on either way, since the computer is the owner's by what the hub knows; nothing is learned
        // from a word of someone else's, a word for another key, or an id its own key didn't sign for.
        QVERIFY(greeted(hub.computers, fingerprint, helloSaying(*hub.stranger, *hub.stranger, laptop)));
        QVERIFY(greeted(hub.computers, fingerprint, helloSaying(*hub.stranger, *hub.first, hub.key(u"other"_s))));
        QJsonObject unsignedClaim = helloSaying(*hub.stranger, *hub.first, laptop);
        unsignedClaim.insert("signature"_L1, HubOwner::signature(*hub.first, HubOwner::message(hub.stranger->id(), laptop)));
        QVERIFY(greeted(hub.computers, fingerprint, unsignedClaim));
        QVERIFY(!hub.computers.isOwner(hub.stranger->id()));
        // A key the hub doesn't know says nothing, whatever it signs with.
        QVERIFY(!greeted(hub.computers, u"SHA256:unknown"_s, helloSaying(*hub.now, *hub.first, laptop)));
        QVERIFY(!hub.computers.isOwner(hub.now->id()));
        QVERIFY(greeted(hub.computers, fingerprint, helloSaying(*hub.now, *hub.first, laptop)));
        QVERIFY(hub.computers.isOwner(hub.now->id()));
        // And what a hub kept before is read as before, the new ids beside it.
        QFile kept(hub.dir.filePath(u"state/computers.json"_s));
        QVERIFY(kept.open(QIODevice::ReadOnly));
        const QJsonObject written = QJsonDocument::fromJson(kept.readAll()).object();
        QCOMPARE(written.value("owner").toString(), hub.first->id());
        QCOMPARE(written.value("owner_ids").toArray().size(), 1);
        QCOMPARE(written.value("computers").toArray().at(0).toObject().value("identity").toString(), hub.first->id());
    }

    void aComputerSaysWhoItWasOnlyWhenItGaveSomeoneUp() {
        const QTemporaryDir dir;
        PersonFile person(dir.filePath(u"person.key"_s));
        QVERIFY(HubOwner::claim(person, u"ssh-ed25519 AAAA"_s).former.isEmpty());
        QVERIFY(!QFile::exists(person.file()));
        QVERIFY(person.get() != nullptr);
        const QString was = person.get()->id();
        QVERIFY(HubOwner::claim(person, u"ssh-ed25519 AAAA"_s).former.isEmpty());
        auto next = Person::generate();
        QVERIFY(next);
        const QString now = next->id();
        QVERIFY(person.adopt(std::move(*next)));
        const HubOwner::Claim claim = HubOwner::claim(person, u"ssh-ed25519 AAAA"_s);
        QCOMPARE(claim.identity, now);
        QVERIFY(HubOwner::signedBy(now, HubOwner::message(now, u"ssh-ed25519 AAAA"_s), claim.signature));
        QVERIFY(HubOwner::vouched({was}, now, u"ssh-ed25519 AAAA"_s, claim.former));
        QVERIFY(!HubOwner::vouched({now}, now, u"ssh-ed25519 AAAA"_s, claim.former));
    }
};

QTEST_GUILESS_MAIN(HubOwnerTests)
#include "hubowner_tests.moc"
