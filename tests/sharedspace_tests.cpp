// Shared spaces (ADR 0001, *People and keys*; #38): spaces several people
// write, their members recorded in the space itself, an operation taken only
// from someone who was a writer at its time; invites only the invitee opens;
// a folder that something else syncs as their transport, holding nothing but
// sealed batches. A household where both write, and a book club where a
// reader can't, end to end, each person's library in a folder of its own.

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

#include "changelog.h"
#include "library.h"
#include "person.h"
#include "replication.h"
#include "sharedspace.h"
#include "sharedspaces.h"
#include "signedops.h"
#include "spacefolder.h"
#include "spaceinvite.h"
#include "spacekey.h"
#include "spacereducer.h"
#include "spacereplay.h"
#include "spaces.h"
#include "subscribedspace.h"

namespace {

using SharedSpace::Role;

template <typename Answer> [[nodiscard]] QString whyNot(const Answer &answer) { return answer ? QString() : answer.error(); }

[[nodiscard]] Person newPerson() {
    auto person = Person::generate();
    if (!person)
        qFatal("%s", qPrintable(person.error()));
    return std::move(*person);
}

// One person's computer: their library and their own Omabook data folder.
struct Someone {
    Someone(const QTemporaryDir &dir, const QString &name) : library(dir.filePath(name + "/Library")) {
        library.usePersonFile(dir.filePath(name + "/home/omabook/person.key"));
        QVERIFY(library.open());
        QVERIFY(library.person() != nullptr);
        id = library.person()->id();
    }

    qint64 book(const QString &title, const QString &isbn) {
        BookMeta meta;
        meta.title = title;
        meta.authors = {"Bo Writer"};
        meta.isbn = isbn;
        return library.addEntry(meta, true).id;
    }

    [[nodiscard]] std::optional<Collection> collection(const QString &name) const { return library.collectionNamed(name); }
    [[nodiscard]] QStringList titlesIn(const QString &name) const {
        QStringList titles;
        if (const auto found = collection(name))
            for (const Book &book : found->of(library.books()))
                titles << book.meta.title;
        titles.sort();
        return titles;
    }

    Library library;
    QString id;
};

// The titles of the books the space holds now, as a member reads it.
[[nodiscard]] QStringList spaceTitles(const Library &library, const QString &space) {
    const ReducedState state = SpaceReducer::reduce(SharedSpace::state(library, space).accepted);
    QStringList titles;
    for (const ReducedBook &book : state.books)
        titles << book.fields.value("title").value;
    titles.sort();
    return titles;
}

[[nodiscard]] SharedSpaces::Synced synced(Someone &who, const QString &space, const QString &folder) {
    auto done = SharedSpaces::sync(who.library, space, folder);
    if (!done)
        qFatal("%s", qPrintable(done.error()));
    return *done;
}

// What an operation says: its thing, field and value.
struct Saying {
    QString entity, id, field, value;
};

// An operation as `author` would write it in `space` from `device`, signed by them.
[[nodiscard]] SpaceOperation signedOp(const Person &author, const QString &space, const QString &device,
                                      std::pair<qint64, qint64> seqAndHlc, const Saying &saying) {
    SpaceOperation op{.space = space,
                      .device = device,
                      .seq = seqAndHlc.first,
                      .hlc = seqAndHlc.second,
                      .entity = saying.entity,
                      .id = saying.id,
                      .field = saying.field,
                      .value = saying.value};
    SignedOps::signAs(op, author);
    return op;
}

[[nodiscard]] SpaceOperation founding(const Person &founder, const QString &space, const QString &nonce, qint64 hlc) {
    return signedOp(
        founder, space, "dev-a", {1, hlc},
        {"space", space, "founded", QString(R"({"founder":"%1","name":"Club","nonce":"%2","parts":[]})").arg(founder.id(), nonce)});
}

[[nodiscard]] qsizetype accepted(const SharedSpace::State &state, const QString &device, qint64 seq) {
    return std::ranges::count_if(state.accepted, [&](const SpaceOperation &op) { return op.device == device && op.seq == seq; });
}

} // namespace

class SharedSpaceTests : public QObject {
    Q_OBJECT

private slots:
    void onlyWhoWasAWriterAtTheTimeWrites() {
        const Person ada = newPerson();
        const Person bo = newPerson();
        const Person cy = newPerson();
        const QString space = SharedSpace::idFor(ada.id(), "n1");
        const QList<SpaceOperation> ops{
            founding(ada, space, "n1", 100),
            signedOp(ada, space, "dev-a", {2, 200}, {"member", bo.id(), "role", "writer"}),
            signedOp(ada, space, "dev-a", {3, 210}, {"member", cy.id(), "role", "reader"}),
            signedOp(bo, space, "dev-b", {1, 300}, {"book", "b:1", "title", "Bo's while a writer"}),
            signedOp(cy, space, "dev-c", {1, 310}, {"book", "b:1", "title", "A reader's"}),
            signedOp(cy, space, "dev-c", {2, 320}, {"member", cy.id(), "role", "writer"}),
            signedOp(ada, space, "dev-a", {4, 400}, {"member", bo.id(), "role", "removed"}),
            signedOp(bo, space, "dev-b", {2, 500}, {"book", "b:1", "title", "Bo's once removed"}),
            signedOp(bo, space, "dev-b", {3, 50}, {"book", "b:1", "title", "Bo's before the space was"}),
        };
        // In any order, the same.
        QList<SpaceOperation> reversed = ops;
        std::ranges::reverse(reversed);
        for (const QList<SpaceOperation> &given : {ops, reversed}) {
            const SharedSpace::State state = SharedSpace::admit(space, given);
            QCOMPARE(state.founder, ada.id());
            QCOMPARE(state.name, QString("Club"));
            QCOMPARE(accepted(state, "dev-b", 1), 1); // written while a writer: stays after the removal
            QCOMPARE(accepted(state, "dev-c", 1), 0); // a reader doesn't write
            QCOMPARE(accepted(state, "dev-c", 2), 0); // nor makes themselves a writer
            QCOMPARE(accepted(state, "dev-b", 2), 0); // after the removal
            QCOMPARE(accepted(state, "dev-b", 3), 0); // before they were a member
            QCOMPARE(state.refused.size(), 4);
            QCOMPARE(state.roleOf(ada.id()), Role::Writer);
            QCOMPARE(state.roleOf(bo.id()), Role::None);
            QCOMPARE(state.roleOf(cy.id()), Role::Reader);
        }
    }

    void aFoundingOrASignatureCantBeForged() {
        const Person ada = newPerson();
        const Person eve = newPerson();
        const QString space = SharedSpace::idFor(ada.id(), "n1");
        // Eve founds Ada's space, saying she is its founder: its id isn't hers.
        const SpaceOperation forged =
            signedOp(eve, space, "dev-e", {1, 50},
                     {"space", space, "founded", QString(R"({"founder":"%1","name":"Mine","nonce":"n1"})").arg(eve.id())});
        SpaceOperation changed = signedOp(ada, space, "dev-a", {2, 200}, {"book", "b:1", "title", "Ada's"});
        changed.value = "Not Ada's";
        SpaceOperation unsigned_ = signedOp(ada, space, "dev-a", {3, 300}, {"book", "b:1", "title", "Ada's"});
        unsigned_.signature.clear();
        unsigned_.signer.clear();
        const SharedSpace::State state = SharedSpace::admit(space, QList{forged, founding(ada, space, "n1", 100), changed, unsigned_});
        QCOMPARE(state.founder, ada.id());
        QCOMPARE(state.accepted.size(), 1);
        QCOMPARE(state.refused.size(), 3);
        // Without its founding, nobody writes a space.
        QVERIFY(
            SharedSpace::admit(space, QList{signedOp(ada, space, "dev-a", {2, 200}, {"book", "b:1", "title", "x"})}).accepted.isEmpty());
    }

    void onlyTheInviteeOpensTheirInvite() {
        const Person ada = newPerson();
        const Person bo = newPerson();
        const Person eve = newPerson();
        const auto key = SpaceKey::generate();
        QVERIFY(key.has_value());
        const SpaceInvite invite{.space = "shared:abc", .name = "Home", .role = "writer", .from = ada.id(), .to = bo.id()};
        const auto file = SpaceInvites::write(ada, invite, *key);
        QVERIFY2(file, qPrintable(whyNot(file)));
        QVERIFY(!file->contains(QByteArray("Home")));
        const auto opened = SpaceInvites::open(bo, *file);
        QVERIFY2(opened, qPrintable(whyNot(opened)));
        QCOMPARE(opened->invite.space, QString("shared:abc"));
        QCOMPARE(opened->invite.from, ada.id());
        QVERIFY(std::ranges::equal(opened->key.bytes(), key->bytes()));
        QVERIFY(!SpaceInvites::open(eve, *file));
        QVERIFY(!SpaceInvites::open(ada, *file));
        QByteArray tampered = *file;
        tampered[tampered.indexOf("\"sealed\": \"") + 20] = tampered.at(tampered.indexOf("\"sealed\": \"") + 20) == 'A' ? 'B' : 'A';
        QVERIFY(!SpaceInvites::open(bo, tampered));
    }

    void aSpaceKeyIsKeptForItsOwnerAlone() {
        QTemporaryDir dir;
        const auto key = SpaceKey::generate();
        QVERIFY(key.has_value());
        const QString file = SpaceKey::fileFor(dir.filePath("spaces"), u"shared:abc");
        QVERIFY(key->keepIn(file).has_value());
        QCOMPARE(QFile::permissions(file) & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther),
                 QFile::Permissions());
        const auto loaded = SpaceKey::load(file);
        QVERIFY2(loaded, qPrintable(whyNot(loaded)));
        QVERIFY(std::ranges::equal(loaded->bytes(), key->bytes()));
        QFile::setPermissions(file, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadOther);
        QVERIFY(!SpaceKey::load(file));
    }

    void aBookClubWhereAReaderCantWriteAndAWritersEditReachesAll() {
        QTemporaryDir dir;
        const QString folder = dir.filePath("Synced");
        QVERIFY(QDir().mkpath(folder));
        Someone ada(dir, "ada"), bo(dir, "bo"), cy(dir, "cy");
        const qint64 dune = ada.book("Dune", "9780441172719");
        const qint64 emma = ada.book("Emma", "9780141439587");
        const auto club = ada.library.addCollection("Book club");
        QVERIFY(club && ada.library.putInCollection(*club, dune, true) && ada.library.putInCollection(*club, emma, true));
        const auto created = SharedSpaces::create(ada.library, "Book club", {});
        QVERIFY2(created, qPrintable(whyNot(created)));
        const QString space = created->space.space;
        QVERIFY(!created->newCollection);
        QVERIFY(created->operations > 0);
        QCOMPARE(spaceTitles(ada.library, space), QStringList({"Dune", "Emma"}));

        const auto toBo = SharedSpaces::invite(ada.library, "Book club", bo.id, Role::Writer, dir.filePath("bo.omabook-invite"));
        const auto toCy = SharedSpaces::invite(ada.library, space, cy.id, Role::Reader, dir.filePath("cy.omabook-invite"));
        QVERIFY2(toBo && toCy, qPrintable(whyNot(toBo) + whyNot(toCy)));
        QCOMPARE(synced(ada, space, folder).sent, SharedSpace::held(ada.library, space).size());

        for (Someone *member : {&bo, &cy}) {
            const auto joined =
                SharedSpaces::join(member->library, dir.filePath(member == &bo ? "bo.omabook-invite" : "cy.omabook-invite"));
            QVERIFY2(joined, qPrintable(whyNot(joined)));
            const SharedSpaces::Synced first = synced(*member, space, folder);
            QVERIFY2(first.refused.isEmpty(), qPrintable(first.refused.join('\n')));
            QVERIFY(first.taken > 0);
            QCOMPARE(spaceTitles(member->library, space), QStringList({"Dune", "Emma"}));
            QCOMPARE(first.filed.collection, QString("Book club"));
            QCOMPARE(first.filed.books.size(), 2);
            QCOMPARE(member->titlesIn("Book club"), QStringList({"Dune", "Emma"}));
        }
        QCOMPARE(SharedSpace::state(cy.library, space).roleOf(cy.id), Role::Reader);
        QCOMPARE(SharedSpace::state(bo.library, space).roleOf(bo.id), Role::Writer);

        // Bo, a writer, puts a book of his own in and corrects a title: both reach everyone.
        const qint64 persuasion = bo.book("Persuasion", "9780141439686");
        QVERIFY(bo.library.putInCollection(bo.collection("Book club")->id, persuasion, true));
        const QList<Book> bosBooks = bo.collection("Book club")->of(bo.library.books());
        const auto emmaHere = std::ranges::find_if(bosBooks, [](const Book &book) { return book.meta.title == "Emma"; });
        QVERIFY(emmaHere != bosBooks.cend());
        const qint64 bosEmma = emmaHere->id;
        BookMeta meta = bo.library.book(bosEmma)->meta;
        QCOMPARE(meta.title, QString("Emma"));
        meta.title = "Emma: A Novel";
        QVERIFY(bo.library.setMeta(bosEmma, meta));
        const SharedSpaces::Synced bos = synced(bo, space, folder);
        QVERIFY(bos.written >= 2);
        QVERIFY(bos.sent > 0);
        for (Someone *member : {&ada, &cy})
            QVERIFY2(synced(*member, space, folder).refused.isEmpty(), "nothing of a writer is refused");
        for (Someone *member : {&ada, &bo, &cy})
            QCOMPARE(spaceTitles(member->library, space), QStringList({"Dune", "Emma: A Novel", "Persuasion"}));
        QCOMPARE(cy.titlesIn("Book club"), QStringList({"Dune", "Emma", "Persuasion"}));
        // Ada's own book keeps her title: the space changes only her collection.
        QCOMPARE(ada.library.book(emma)->meta.title, QString("Emma"));

        // Cy, a reader, can't write: her own changes stay hers.
        const qint64 hers = cy.book("Middlemarch", "9780141439549");
        QVERIFY(cy.library.putInCollection(cy.collection("Book club")->id, hers, true));
        const SharedSpaces::Synced cys = synced(cy, space, folder);
        QCOMPARE(cys.written, 0);
        QCOMPARE(cys.sent, 0);
        QVERIFY(!SharedSpace::setRole(cy.library, space, cy.id, Role::Writer));
        const ChangeLog::Entry entry{.space = space, .entity = "book", .id = "b:x", .field = "title", .value = "Mine", .hlc = {}};
        const auto refused = SharedSpace::write(cy.library, space, std::span(&entry, 1));
        QVERIFY(!refused && refused.error().contains("reader"));

        // A batch she writes by hand, signed by her and sealed with the key, is refused by everyone.
        const Person *cyPerson = cy.library.person();
        const auto key = SpaceKey::load(SharedSpaces::keyFile(cy.library, space));
        QVERIFY(key.has_value());
        const QString cyDevice = Spaces::deviceId(cy.library);
        const SpaceOperation forged =
            signedOp(*cyPerson, space, cyDevice, {1, Spaces::tickHlc(cy.library)}, {"book", "b:x", "title", "Forged"});
        QVERIFY(SpaceFolder::write(folder, space, *key, cyDevice, QList{forged}).has_value());
        const SharedSpaces::Synced adas = synced(ada, space, folder);
        QCOMPARE(adas.taken, 0);
        QCOMPARE(adas.refused.size(), 1);
        QVERIFY(adas.refused.first().contains("reader"));
        QVERIFY(!spaceTitles(ada.library, space).contains("Forged"));
    }

    void aHouseholdWhereBothWrite() {
        QTemporaryDir dir;
        const QString folder = dir.filePath("Share");
        QVERIFY(QDir().mkpath(folder));
        Someone ada(dir, "ada"), bo(dir, "bo");
        const qint64 hobbit = ada.book("The Hobbit", "9780547928227");
        const auto created = SharedSpaces::create(ada.library, "Home", {}, "Our books");
        QVERIFY2(created, qPrintable(whyNot(created)));
        QVERIFY(created->newCollection);
        const QString space = created->space.space;
        QVERIFY(ada.library.putInCollection(ada.collection("Our books")->id, hobbit, true));
        QVERIFY(SharedSpaces::invite(ada.library, "Home", bo.id, Role::Writer, dir.path()).has_value());
        std::ignore = synced(ada, space, folder);
        QString invite;
        for (QDirIterator files(dir.path(), {"*.omabook-invite"}); files.hasNext();)
            invite = files.next();
        const qint64 bosHobbit = bo.book("The Hobbit", "9780547928227");
        const qint64 dune = bo.book("Dune", "9780441172719");
        QVERIFY(SharedSpaces::join(bo.library, invite, "Shelves").has_value());
        std::ignore = synced(bo, space, folder);
        // Bo had the book already: it is his own that is put in.
        QCOMPARE(bo.library.collectionNamed("Shelves")->of(bo.library.books()).first().id, bosHobbit);
        QVERIFY(bo.library.putInCollection(bo.collection("Shelves")->id, dune, true));
        std::ignore = synced(bo, space, folder);
        const qint64 adasBefore = ada.library.books().size();
        std::ignore = synced(ada, space, folder);
        QCOMPARE(ada.titlesIn("Our books"), QStringList({"Dune", "The Hobbit"}));
        QCOMPARE(ada.library.books().size(), adasBefore + 1); // Dune, as a wish of hers
        // Both write; each change reaches the other, and the space has one Hobbit.
        QVERIFY(ada.library.putInCollection(ada.collection("Our books")->id, hobbit, false));
        std::ignore = synced(ada, space, folder);
        std::ignore = synced(bo, space, folder);
        QCOMPARE(spaceTitles(bo.library, space), spaceTitles(ada.library, space));
        const ReducedState state = SpaceReducer::reduce(SharedSpace::state(bo.library, space).accepted);
        QCOMPARE(state.books.size(), 2);
        // The households' own rows never take the space's operations, nor do they go by ssh.
        for (const SpaceOperation &op : SpaceReplay::operations(ada.library) + Replication::operations(ada.library))
            QVERIFY(!op.space.startsWith("shared:"));
        QVERIFY(SharedSpaces::list(bo.library).size() >= 2);
    }

    void aTamperedBatchIsRefusedWhole() {
        QTemporaryDir dir;
        const QString folder = dir.filePath("Share");
        QVERIFY(QDir().mkpath(folder));
        Someone ada(dir, "ada"), bo(dir, "bo");
        QVERIFY(ada.book("Dune", "9780441172719") > 0);
        const auto created = SharedSpaces::create(ada.library, "Home", {});
        QVERIFY(created.has_value());
        const QString space = created->space.space;
        QVERIFY(SharedSpaces::invite(ada.library, space, bo.id, Role::Reader, dir.filePath("bo.omabook-invite")).has_value());
        std::ignore = synced(ada, space, folder);
        // Nothing in the folder is plaintext.
        for (QDirIterator files(folder, QDir::Files, QDirIterator::Subdirectories); files.hasNext();) {
            QFile file(files.next());
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QByteArray bytes = file.readAll();
            QVERIFY(!bytes.contains("Home") && !bytes.contains("founded") && !bytes.contains(ada.id.toUtf8()));
            file.close();
            QFile out(file.fileName());
            QVERIFY(out.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QByteArray changed = bytes;
            changed[changed.size() / 2] = static_cast<char>(changed.at(changed.size() / 2) ^ 1);
            out.write(changed);
        }
        QVERIFY(SharedSpaces::join(bo.library, dir.filePath("bo.omabook-invite")).has_value());
        const SharedSpaces::Synced bos = synced(bo, space, folder);
        QCOMPARE(bos.taken, 0);
        QCOMPARE(bos.refused.size(), 1);
        QVERIFY(SharedSpace::held(bo.library, space).isEmpty());
    }
};

QTEST_GUILESS_MAIN(SharedSpaceTests)
#include "sharedspace_tests.moc"
