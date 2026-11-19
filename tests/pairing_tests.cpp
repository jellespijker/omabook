// Signed operations in the log, and pairing a computer into its person's
// personal space (ADR 0001, *People and keys*). Every key here is made on the
// spot in a temporary folder: no real key is ever read.

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <cstddef>
#include <memory>
#include <sodium.h>
#include <utility>

#include "crypto.h"
#include "fixtures.h"
#include "graph.h"
#include "graphcontext.h"
#include "hlc.h"
#include "library.h"
#include "pairing.h"
#include "person.h"
#include "publishedspace.h"
#include "remote.h"
#include "replication.h"
#include "sharedkeys.h"
#include "sharedspace.h"
#include "sharedspaces.h"
#include "signedops.h"
#include "spacecarry.h"
#include "spacekey.h"
#include "spacelinks.h"
#include "spacereducer.h"
#include "spacereplay.h"
#include "spaces.h"
#include "subscribedspace.h"
#include "subscriptions.h"
#include "sync.h"
#include "synclog.h"

namespace {

// Where the stand-in ssh's user (Fixtures::standInSsh) keeps their person.
QString thereKey(const QTemporaryDir &dir) { return dir.filePath("there-home/.local/share/omabook/person.key"); }

// The operations of `library` that `device` wrote, as they travel.
QList<SpaceOperation> writtenBy(const Library &library, const QString &device) {
    QList<SpaceOperation> ops = Replication::operations(library);
    ops.removeIf([&device](const SpaceOperation &op) { return op.device != device; });
    return ops;
}

// The last operation `library` wrote itself of `field` (of any, when empty),
// in the personal space it now writes in.
SpaceOperation lastWritten(const Library &library, const QString &field = {}) {
    QList<SpaceOperation> ops = SpaceReplay::operations(library);
    ops.removeIf([&](const SpaceOperation &op) {
        return op.device != Spaces::deviceId(library) || op.space != Spaces::personalSpace(library)
               || (!field.isEmpty() && op.field != field);
    });
    return ops.isEmpty() ? SpaceOperation{} : *std::ranges::max_element(ops, {}, &SpaceOperation::seq);
}

using SharedSpace::Role;

template <typename Answer> [[nodiscard]] QString whyNot(const Answer &answer) { return answer ? QString() : answer.error(); }

// A book in the collection `collection` (made when there is none).
void fileBook(Library &library, const QString &collection, const QString &title, const QString &isbn) {
    BookMeta meta;
    meta.title = title;
    meta.authors = {"Bo Writer"};
    meta.isbn = isbn;
    const qint64 book = library.addEntry(meta, true).id;
    const auto existing = library.collectionNamed(collection);
    const auto into = existing ? std::expected<qint64, QString>(existing->id) : library.addCollection(collection);
    QVERIFY2(into && library.putInCollection(*into, book, true), qPrintable(whyNot(into)));
}

// One person's computer: a library and the user's own Omabook data folder.
struct Computer {
    Computer(const QTemporaryDir &dir, const QString &name) : library(dir.filePath(name + "/Library")) {
        library.usePersonFile(dir.filePath(name + "/home/omabook/person.key"));
        QVERIFY(library.open());
        QVERIFY(library.person() != nullptr);
    }

    [[nodiscard]] QString id() const { return library.person()->id(); }

    // A book in the collection `collection` (made when there is none).
    void file(const QString &collection, const QString &title, const QString &isbn) { fileBook(library, collection, title, isbn); }

    [[nodiscard]] SharedSpaces::Synced sync(const QString &space, const QString &folder) {
        auto done = SharedSpaces::sync(library, space, folder);
        if (!done)
            qFatal("%s", qPrintable(done.error()));
        return *done;
    }

    Library library;
};

[[nodiscard]] Person newPerson() {
    auto person = Person::generate();
    if (!person)
        qFatal("%s", qPrintable(person.error()));
    return std::move(*person);
}

// A person whose id is smaller than `than`: the one a pairing keeps.
[[nodiscard]] Person keptOver(const QString &than) {
    for (;;)
        if (Person person = newPerson(); person.id() < than)
            return person;
}

// Pairs `computer` with another of its person's, whose person `kept` is:
// what the replication over ssh hands it (Pairing::take).
[[nodiscard]] bool pairWith(Computer &computer, const Person &kept) {
    QByteArray secret = kept.secretForPairing();
    const auto taken = Pairing::take(computer.library, {{"signs", 1}, {"person", kept.id()}, {"person_key", QString::fromLatin1(secret)}});
    sodium_memzero(secret.data(), static_cast<std::size_t>(secret.size()));
    return taken && *taken && computer.id() == kept.id();
}

// The titles of the books a shared space holds, as a member reads it.
[[nodiscard]] QStringList spaceTitles(const Library &library, const QString &space) {
    QStringList titles;
    for (const ReducedBook &book : SpaceReducer::reduce(SharedSpace::state(library, space).accepted).books)
        titles << book.fields.value("title").value;
    titles.sort();
    return titles;
}

// Two computers of one person, this one and the one the stand-in ssh reaches
// (Fixtures::standInSsh), which runs Omabook there in a home of its own.
struct OwnComputers {
    explicit OwnComputers(const QTemporaryDir &dir)
        : folder(dir.path()), remote("desktop:" + dir.filePath("There"), {Fixtures::standInSsh(dir.path(), OMABOOK_BINARY)}),
          here(dir.filePath("Here")) {
        here.usePersonFile(dir.filePath("here-home/omabook/person.key"));
        QVERIFY(here.open());
        QVERIFY(there() != nullptr);
    }

    // The library there as it is now, opened anew: a sync there may have made it another person.
    [[nodiscard]] std::unique_ptr<Library> there() const {
        auto library = std::make_unique<Library>(folder + "/There");
        library->usePersonFile(folder + "/there-home/.local/share/omabook/person.key");
        return library->open() ? std::move(library) : nullptr;
    }

    // Syncs here with there, as `omabook sync` does: what it said went wrong.
    [[nodiscard]] QStringList sync() {
        const SyncSummary summary = syncLibrary(here, remote);
        return summary.error.isEmpty() ? summary.problems : QStringList{summary.error};
    }

    QString folder;
    Remote remote;
    Library here;
};

// A book of a file of its own, `title`, in `library`, as with `omabook add`.
[[nodiscard]] qint64 bookOfFile(const QTemporaryDir &dir, Library &library, const QString &title) {
    const QString file = dir.filePath(title + ".epub");
    if (!QFile::exists(file) && !Fixtures::epub(file, title))
        return 0;
    return library.add(file).id;
}

[[nodiscard]] qint64 bookTitled(const Library &library, const QString &title) {
    const QList<Book> books = library.books();
    const auto found = std::ranges::find_if(books, [&title](const Book &book) { return book.meta.title == title; });
    return found != books.cend() ? found->id : 0;
}

[[nodiscard]] QStringList layerNames(const Library &library, qint64 book) {
    QStringList names;
    for (const GraphLayers::Layer &layer : GraphContext::layers(library))
        for (const GraphNode &node : layer.ofBook(book).nodes)
            names << node.name;
    names.sort();
    return names;
}

[[nodiscard]] QString linkedPath(const Library &library, const QString &space) {
    const auto collection = library.collection(SubscribedSpace::linkedCollection(library, space));
    return collection ? collection->path : QString();
}

// A collection published by Cy, another person: "Out", with Dune, and what
// Cy's graph says of it early on and later.
[[nodiscard]] SpaceSnapshot cysCollection(const QTemporaryDir &dir) {
    Computer cy(dir, "cy");
    const qint64 dune = bookOfFile(dir, cy.library, "Dune");
    const auto out = cy.library.addCollection("Out");
    if (dune == 0 || !out || !cy.library.putInCollection(*out, dune, true))
        qFatal("Cy's collection could not be made");
    Graph graph(cy.library);
    if (graph.upsert({}, "character", "Paul", "The duke's son.", {}, GraphPlace{dune, 0, 0.1}, "Paul") <= 0
        || graph.upsert({}, "character", "Alia", "Born later.", {}, GraphPlace{dune, 0, 0.8}, "Alia") <= 0)
        qFatal("Cy's graph could not be made");
    cy.library.setProgress(dune, {.section = 0, .fraction = 1.0, .percent = 100}); // read through: all of it goes
    auto snapshot = PublishedSpace::snapshot(cy.library, *out, {.graph = true}, {.name = "Cy", .person = cy.id()});
    if (!snapshot)
        qFatal("%s", qPrintable(snapshot.error()));
    return *snapshot;
}

[[nodiscard]] QString roleShown(const Library &library, const QString &space) {
    for (const SharedSpaces::Summary &summary : SharedSpaces::list(library))
        if (summary.space == space)
            return summary.role;
    return {};
}

} // namespace

class PairingTests : public QObject {
    Q_OBJECT

private slots:
    void everyOperationWrittenIsSignedAsItIsKept() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("book.epub"), "Signed For"));
        Library library(dir.filePath("Library"));
        library.usePersonFile(dir.filePath("home/person.key"));
        QVERIFY(library.open());
        const qint64 book = library.add(dir.filePath("book.epub")).id;
        library.setProgress(book, {.section = 2, .fraction = 0.25, .percent = 20, .updated = 1794860100});
        const QList<SpaceOperation> ops = SpaceReplay::operations(library);
        QVERIFY(!ops.isEmpty());
        // The author is the person, the device this computer, and the bytes
        // kept are the bytes signed.
        for (const SpaceOperation &op : ops) {
            QCOMPARE(op.author, library.person()->id());
            QCOMPARE(op.signer, library.person()->id());
            QCOMPARE(op.device, Spaces::deviceId(library));
            QVERIFY2(SignedOps::verify(op), qPrintable(op.field));
        }
        // Changed in any part, it is no longer what was signed.
        SpaceOperation changed = lastWritten(library);
        QVERIFY(SignedOps::verify(changed));
        changed.hlc += 1;
        QVERIFY(!SignedOps::verify(changed));
        SpaceOperation claimed = lastWritten(library);
        claimed.author = Spaces::deviceId(library);
        QVERIFY(!SignedOps::verify(claimed));

        // The person is kept per user, made for the owner alone.
        QVERIFY(QFile::exists(dir.filePath("home/person.key")));
        QVERIFY(!QFile::exists(Person::keyFile(library.root())));
    }

    void aKeyBesideTheLibraryMovesToTheUsersFolderOnce() {
        QTemporaryDir dir;
        const QString root = dir.filePath("Library");
        QVERIFY(QDir().mkpath(root));
        const auto before = Person::loadOrCreate(Person::keyFile(root));
        QVERIFY(before);
        PersonFile kept(dir.filePath("home/omabook/person.key"));
        const Person *person = kept.get(root);
        QVERIFY(person);
        QCOMPARE(person->id(), before->id());
        QVERIFY(!QFile::exists(Person::keyFile(root)));
        QCOMPARE(QFile::permissions(kept.file()) & ~(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser),
                 QFile::Permissions());
        // One there already stays: a key beside a library never replaces the user's.
        QVERIFY(Person::loadOrCreate(Person::keyFile(root)));
        PersonFile again(kept.file());
        QCOMPARE(again.get(root)->id(), before->id());
    }

    void aPairingSecretIsTheSamePersonAndNothingElseIsOne() {
        const auto person = Person::generate();
        QVERIFY(person);
        const auto same = Person::fromPairingSecret(person->secretForPairing());
        QVERIFY(same);
        QCOMPARE(same->id(), person->id());
        QVERIFY(!Person::fromPairingSecret("not a key"));
        QVERIFY(!Person::fromPairingSecret(person->secretForPairing().left(20)));
    }

    void pairingTwoComputersMakesThemOnePersonAndTheSignaturesTravel_data() {
        QTest::addColumn<bool>("smallerHere");
        QTest::newRow("the smaller id here: its key goes there") << true;
        QTest::newRow("the smaller id there: its key comes here") << false;
    }

    void pairingTwoComputersMakesThemOnePersonAndTheSignaturesTravel() {
        QFETCH(bool, smallerHere);
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("shared.epub"), "On Both"));
        QVERIFY(Fixtures::epub(dir.filePath("there.epub"), "Only There"));
        const Remote remote("laptop:" + dir.filePath("There"), {Fixtures::standInSsh(dir.path(), OMABOOK_BINARY)});
        auto first = Person::generate();
        auto second = Person::generate();
        QVERIFY(first && second);
        if ((first->id() < second->id()) != smallerHere)
            std::swap(first, second);
        QVERIFY(first->keepIn(dir.filePath("here-home/person.key")) && second->keepIn(thereKey(dir)));
        Library here(dir.filePath("Here"));
        here.usePersonFile(dir.filePath("here-home/person.key"));
        Library there(dir.filePath("There"));
        there.usePersonFile(thereKey(dir));
        QVERIFY(here.open() && there.open());
        const QString hereWas = here.person()->id();
        const QString thereWas = there.person()->id();
        QCOMPARE(hereWas < thereWas, smallerHere);
        const qint64 sharedHere = here.add(dir.filePath("shared.epub")).id;
        const qint64 sharedThere = there.add(dir.filePath("shared.epub")).id;
        QVERIFY(there.add(dir.filePath("there.epub")).error.isEmpty());
        there.setProgress(sharedThere, {.section = 4, .fraction = 0.5, .percent = 40, .updated = 1794860200});

        const SyncSummary paired = syncLibrary(here, remote);
        QVERIFY2(paired.error.isEmpty(), qPrintable(paired.error));
        QVERIFY2(paired.problems.isEmpty(), qPrintable(paired.problems.join('\n')));
        QCOMPARE(paired.refused, 0);
        QCOMPARE(here.progress(sharedHere).section, 4);

        // One personal space, and one person: the one of the two with the smaller id.
        const QString one = std::min(hereWas, thereWas);
        QCOMPARE(Spaces::personalSpace(here), Spaces::personalSpace(there));
        QCOMPARE(here.person()->id(), one);
        QCOMPARE(Person::load(here.personFile().file())->id(), one);
        QCOMPARE(Person::load(thereKey(dir))->id(), one);
        QCOMPARE(Spaces::computers(here).size(), 2);
        QCOMPARE(Spaces::computers(there).size(), 2);

        // What there wrote came signed, by the person it was then, and verified.
        const QList<SpaceOperation> fromThere = writtenBy(here, Spaces::deviceId(there));
        QVERIFY(!fromThere.isEmpty());
        for (const SpaceOperation &op : fromThere) {
            QCOMPARE(op.signer, thereWas);
            QVERIFY(SignedOps::verify(op));
        }

        // From now on both sign as the one person.
        here.setProgress(sharedHere, {.section = 5, .fraction = 0.1, .percent = 50, .updated = 1794860300});
        QCOMPARE(lastWritten(here, "progress").signer, one);
        Library thereAgain(dir.filePath("There"));
        thereAgain.usePersonFile(thereKey(dir));
        QVERIFY(thereAgain.open());
        thereAgain.setProgress(sharedThere, {.section = 6, .fraction = 0.1, .percent = 60, .updated = 1794860400});
        QCOMPARE(lastWritten(thereAgain, "progress").signer, one);
        const SyncSummary settled = syncLibrary(here, remote);
        QVERIFY2(settled.problems.isEmpty(), qPrintable(settled.problems.join('\n')));
        QCOMPARE(here.progress(sharedHere).section, 6);
        QCOMPARE(Replication::operations(here), Replication::operations(thereAgain));
        QCOMPARE(Person::load(here.personFile().file())->id(), one);

        // `omabook identity` there says who it is, and the computers paired.
        QProcess identity;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert("XDG_DATA_HOME", dir.filePath("there-home/.local/share"));
        identity.setProcessEnvironment(environment);
        identity.start(OMABOOK_BINARY, {"identity", "--json", "--library", dir.filePath("There")});
        QVERIFY(identity.waitForFinished(30000));
        const QJsonObject shown = QJsonDocument::fromJson(identity.readAllStandardOutput()).object();
        QCOMPARE(shown.value("id").toString(), one);
        QCOMPARE(shown.value("computers").toArray().size(), 2);
        QVERIFY(shown.value("computers").toArray().first().toObject().value("this").toBool());
    }

    void aTamperedOperationFromAPeerIsRefusedAndNeverApplied() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("shared.epub"), "Kept Honest"));
        const Remote remote("laptop:" + dir.filePath("There"), {Fixtures::standInSsh(dir.path(), OMABOOK_BINARY)});
        Library here(dir.filePath("Here"));
        here.usePersonFile(dir.filePath("here-home/person.key"));
        Library there(dir.filePath("There"));
        there.usePersonFile(thereKey(dir));
        QVERIFY(here.open() && there.open());
        const qint64 sharedHere = here.add(dir.filePath("shared.epub")).id;
        const qint64 sharedThere = there.add(dir.filePath("shared.epub")).id;
        there.setProgress(sharedThere, {.section = 2, .fraction = 0.5, .percent = 20, .updated = 1794860200});
        QVERIFY(syncLibrary(here, remote).problems.isEmpty());
        QCOMPARE(here.progress(sharedHere).section, 2);

        // An operation there that is not as it was signed: a later place,
        // signed, and then changed on its way.
        Library thereAgain(dir.filePath("There"));
        thereAgain.usePersonFile(thereKey(dir));
        QVERIFY(thereAgain.open());
        const QList<SpaceOperation> thereWrote = writtenBy(thereAgain, Spaces::deviceId(thereAgain));
        const auto place = std::ranges::find(thereWrote, QString("progress"), &SpaceOperation::field);
        QVERIFY(place != thereWrote.cend());
        SpaceOperation forged = *place;
        forged.seq = 1'000'000; // far after anything there wrote in its space
        forged.hlc = Hlc::pack(1794860900, 0);
        SignedOps::signAs(forged, *thereAgain.person());
        QVERIFY(SignedOps::verify(forged));
        forged.value = R"({"fraction":0.9,"percent":99,"section":9})";
        QVERIFY(!SignedOps::verify(forged));
        QVERIFY(SpaceReplay::apply(thereAgain, std::span(&forged, 1)));

        const SyncSummary summary = syncLibrary(here, remote);
        QCOMPARE(summary.refused, 1);
        QCOMPARE(summary.problems.size(), 1);
        QVERIFY2(summary.problems.first().contains("refused"), qPrintable(summary.problems.first()));
        QVERIFY(summary.toJson().value("refused").toInt() == 1);
        // Never taken in: not in the log here, and the place is as it was.
        QCOMPARE(here.progress(sharedHere).section, 2);
        const QList<SpaceOperation> taken = writtenBy(here, forged.device);
        QVERIFY(std::ranges::none_of(taken, [&forged](const SpaceOperation &op) { return op.seq == forged.seq; }));
        // And refused again the next time, never taken by being sent twice.
        QCOMPARE(syncLibrary(here, remote).refused, 1);
        QCOMPARE(here.progress(sharedHere).section, 2);
    }

    void aPublishedSpaceCarriesNoSignatureOfHome() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("book.epub"), "Shared Out"));
        Library library(dir.filePath("Library"));
        library.usePersonFile(dir.filePath("home/person.key"));
        QVERIFY(library.open());
        QVERIFY(library.add(dir.filePath("book.epub")).error.isEmpty());
        const auto shelf = library.addCollection("Out", "Shared");
        QVERIFY(shelf);
        QVERIFY(std::ranges::all_of(SpaceReplay::operations(library), [](const SpaceOperation &op) { return SignedOps::verify(op); }));
        // Each operation as the space has it (its own space, devices and
        // numbers) is not what was signed at home, so it goes without that signature.
        const auto published = PublishedSpace::snapshot(library, *shelf, {}, {.name = "Ada", .person = library.person()->id()});
        QVERIFY2(published, qPrintable(published ? QString() : published.error()));
        QVERIFY(!published->operations.isEmpty());
        for (const SpaceOperation &op : published->operations)
            QVERIFY(op.signer.isEmpty() && op.signature.isEmpty() && op.author == library.person()->id());
    }

    void unsignedOperationsAreTakenOnlyFromOnesOwnComputers() {
        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        library.usePersonFile(dir.filePath("home/person.key"));
        QVERIFY(library.open());
        // As an Omabook from before signing writes them, and sends them: nine parts, no signature.
        const SpaceOperation old{.space = "personal:older-laptop",
                                 .device = "older-laptop",
                                 .seq = 1,
                                 .hlc = Hlc::pack(1794860000, 0),
                                 .author = "older-laptop",
                                 .entity = "collection",
                                 .id = "c:From Before",
                                 .field = "name",
                                 .value = "From Before"};
        const QJsonArray sent = Replication::toJson(std::span(&old, 1), false);
        QCOMPARE(sent.first().toArray().size(), 9);
        const QList<SpaceOperation> received = Replication::operationsFrom(sent);
        QCOMPARE(received.size(), 1);
        QCOMPARE(Replication::admit(library, received, "older-laptop").refused, 1);
        QCOMPARE(Replication::admit(library, received, "older-laptop").ops.size(), 0);
        // One of one's own computers, paired: taken, for an Omabook from before.
        Spaces::pair(library, "personal:older-laptop", "older-laptop", "laptop");
        QCOMPARE(Replication::admit(library, received, "older-laptop").ops.size(), 1);

        // An answer to such a computer gives it the nine parts it knows.
        const QJsonObject answer = SyncLog::answer(
            library,
            {{"space", "personal:older-laptop"}, {"device", "older-laptop"}, {"vector", QJsonObject()}, {"ops", sent}, {"dry_run", false}});
        QCOMPARE(answer.value("taken").toObject().value("refused").toInt(), 0);
        QVERIFY(!answer.value("ops").toArray().isEmpty());
        for (const auto &op : answer.value("ops").toArray())
            QCOMPARE(op.toArray().size(), 9);
        QVERIFY(!answer.contains("person_key"));
        QVERIFY(std::ranges::any_of(SpaceReplay::operations(library), [](const SpaceOperation &op) { return op.id == "c:From Before"; }));
    }

    // #50: a computer that joined a shared space, then was paired and took on
    // another person, keeps its place there: a writer goes on writing (the
    // person it was makes the one it is a writer), a reader goes on reading.
    void aMemberOfASharedSpaceKeepsItsPlaceAfterPairing_data() {
        QTest::addColumn<int>("role");
        QTest::newRow("a writer") << static_cast<int>(Role::Writer);
        QTest::newRow("a reader") << static_cast<int>(Role::Reader);
    }

    void aMemberOfASharedSpaceKeepsItsPlaceAfterPairing() {
        QFETCH(int, role);
        const auto given = static_cast<Role>(role);
        QTemporaryDir dir;
        const QString folder = dir.filePath("Synced");
        QVERIFY(QDir().mkpath(folder));
        Computer ada(dir, "ada");
        Computer bo(dir, "bo");
        ada.file("Home", "Dune", "9780441172719");
        const auto created = SharedSpaces::create(ada.library, "Home", {});
        QVERIFY2(created, qPrintable(whyNot(created)));
        const QString space = created->space.space;
        const QString boWas = bo.id();
        QVERIFY(SharedSpaces::invite(ada.library, space, boWas, given, dir.filePath("bo.omabook-invite")).has_value());
        std::ignore = ada.sync(space, folder);
        QVERIFY(SharedSpaces::join(bo.library, dir.filePath("bo.omabook-invite")).has_value());
        QVERIFY(bo.sync(space, folder).refused.isEmpty());

        const Person kept = keptOver(boWas);
        QVERIFY(pairWith(bo, kept));
        // The person given up is kept, for its owner alone, and still opens and signs.
        const QString former = bo.library.personFile().formerFile(boWas);
        QCOMPARE(QFile::permissions(former) & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther),
                 QFile::Permissions());
        QCOMPARE(Person::load(former)->id(), boWas);
        QCOMPARE(bo.library.personFile().formerIds(), QStringList{boWas});

        // Bo goes on reading what Ada adds.
        ada.file("Home", "Emma", "9780141439587");
        std::ignore = ada.sync(space, folder);
        const SharedSpaces::Synced bos = bo.sync(space, folder);
        QVERIFY2(bos.refused.isEmpty(), qPrintable(bos.refused.join('\n')));
        QCOMPARE(spaceTitles(bo.library, space), QStringList({"Dune", "Emma"}));
        QCOMPARE(roleShown(bo.library, space), SharedSpace::roleName(given));

        if (given == Role::Reader) {
            // A reader can make nobody a member: Bo stays the reader he was, as who he was.
            QCOMPARE(bos.sent, 0);
            const SharedSpace::State state = SharedSpace::state(ada.library, space);
            QCOMPARE(state.roleOf(boWas), Role::Reader);
            QCOMPARE(state.roleOf(kept.id()), Role::None);
            QCOMPARE(SharedSpace::memberAs(bo.library, SharedSpace::state(bo.library, space)), boWas);
            const ChangeLog::Entry entry{.space = space, .entity = "book", .id = "b:x", .field = "title", .value = "Mine", .hlc = {}};
            QVERIFY(!SharedSpace::write(bo.library, space, std::span(&entry, 1)));
            return;
        }
        // A writer goes on writing, as the person he is now, and everyone takes it.
        bo.file("Home", "Persuasion", "9780141439686");
        const SharedSpaces::Synced wrote = bo.sync(space, folder);
        QVERIFY(wrote.written > 0);
        const SharedSpaces::Synced adas = ada.sync(space, folder);
        QVERIFY2(adas.refused.isEmpty(), qPrintable(adas.refused.join('\n')));
        QCOMPARE(spaceTitles(ada.library, space), QStringList({"Dune", "Emma", "Persuasion"}));
        const SharedSpace::State state = SharedSpace::state(ada.library, space);
        QCOMPARE(state.roleOf(kept.id()), Role::Writer);
        QCOMPARE(state.roleOf(boWas), Role::Writer); // another of his computers may still be him
        const auto persuasion = std::ranges::find(state.accepted, QString("Persuasion"), &SpaceOperation::value);
        QVERIFY(persuasion != state.accepted.cend());
        QCOMPARE(persuasion->author, kept.id());
        // Once is enough: the next sync hands nothing over again.
        QCOMPARE(SharedSpace::carryOver(bo.library, space).value_or(true), false);
    }

    // The other order: invited as who it was, it joins only after the pairing.
    void anInviteToWhoAComputerWasIsJoinedAfterPairing() {
        QTemporaryDir dir;
        const QString folder = dir.filePath("Synced");
        QVERIFY(QDir().mkpath(folder));
        Computer ada(dir, "ada");
        Computer bo(dir, "bo");
        ada.file("Home", "Dune", "9780441172719");
        const auto created = SharedSpaces::create(ada.library, "Home", {});
        QVERIFY(created.has_value());
        const QString space = created->space.space;
        const QString boWas = bo.id();
        QVERIFY(SharedSpaces::invite(ada.library, space, boWas, Role::Writer, dir.filePath("bo.omabook-invite")).has_value());
        std::ignore = ada.sync(space, folder);

        const Person kept = keptOver(boWas);
        QVERIFY(pairWith(bo, kept));
        const auto joined = SharedSpaces::join(bo.library, dir.filePath("bo.omabook-invite"));
        QVERIFY2(joined, qPrintable(whyNot(joined)));
        QVERIFY(bo.sync(space, folder).refused.isEmpty());
        QCOMPARE(spaceTitles(bo.library, space), QStringList({"Dune"}));
        bo.file("Home", "Persuasion", "9780141439686");
        QVERIFY(bo.sync(space, folder).written > 0);
        QVERIFY(ada.sync(space, folder).refused.isEmpty());
        QCOMPARE(spaceTitles(ada.library, space), QStringList({"Dune", "Persuasion"}));
        QCOMPARE(SharedSpace::state(ada.library, space).roleOf(kept.id()), Role::Writer);
        // An invite to someone this computer never was still isn't opened.
        const Person eve = newPerson();
        QVERIFY(SharedSpaces::invite(ada.library, space, eve.id(), Role::Reader, dir.filePath("eve.omabook-invite")).has_value());
        QVERIFY(!SharedSpaces::join(bo.library, dir.filePath("eve.omabook-invite")));
    }

    // A space published before pairing is the same space after it: its owner
    // moves to the person kept, and those who follow it take what is new.
    void aSpacePublishedBeforePairingIsStillSharedAfter() {
        QTemporaryDir dir;
        Computer bo(dir, "bo");
        Computer cy(dir, "cy");
        bo.file("Out", "Dune", "9780441172719");
        const qint64 out = bo.library.collectionNamed("Out")->id;
        const QString boWas = bo.id();
        const auto first = PublishedSpace::snapshot(bo.library, out, {}, {.name = "Bo", .person = boWas});
        QVERIFY2(first, qPrintable(whyNot(first)));
        QVERIFY(Subscriptions::subscribe(cy.library, *first, {}).has_value());

        const Person kept = keptOver(boWas);
        QVERIFY(pairWith(bo, kept));
        bo.file("Out", "Emma", "9780141439587");
        const auto later = PublishedSpace::snapshot(bo.library, out, {}, {.name = "Bo", .person = kept.id()});
        QVERIFY2(later, qPrintable(whyNot(later)));
        QCOMPARE(later->space, first->space);
        QCOMPARE(later->owner.person, kept.id());
        QCOMPARE(later->owner.name, QString("Bo"));
        const auto followed = Subscriptions::subscribe(cy.library, *later, {});
        QVERIFY2(followed, qPrintable(whyNot(followed)));
        QCOMPARE(followed->space.owner, kept.id());
        QCOMPARE(SpaceReducer::reduce(SubscribedSpace::operations(cy.library, first->space)).books.size(), 2);
        // Spaces::recorded names one space, owned by the person kept.
        const QList<Spaces::Recorded> recorded = Spaces::recorded(bo.library);
        QCOMPARE(std::ranges::count(recorded, QString("published"), &Spaces::Recorded::kind), 1);

        // Someone else saying it is theirs now, without its owner handing it on, is refused.
        SpaceSnapshot taken = *later;
        const Person eve = newPerson();
        taken.owner.person = eve.id();
        for (SpaceOperation &op : taken.operations)
            op.author = eve.id();
        QVERIFY(!SubscribedSpace::verify(cy.library, taken));
    }

    // #51: a space joined on one of one's computers is on the others after
    // their next sync over ssh, key and all, and they read and write it.
    void aSharedSpaceJoinedOnOneComputerIsOnTheOtherAfterASync() {
        QTemporaryDir dir;
        const QString folder = dir.filePath("Synced");
        QVERIFY(QDir().mkpath(folder));
        OwnComputers bo(dir);
        QVERIFY2(bo.sync().isEmpty(), "the first sync pairs");
        const QString person = bo.here.person()->id();
        QCOMPARE(bo.there()->person()->id(), person);

        Computer ada(dir, "ada");
        ada.file("Home", "Dune", "9780441172719");
        const auto created = SharedSpaces::create(ada.library, "Home", {});
        QVERIFY2(created, qPrintable(whyNot(created)));
        const QString space = created->space.space;
        QVERIFY(SharedSpaces::invite(ada.library, space, person, Role::Writer, dir.filePath("bo.omabook-invite")).has_value());
        std::ignore = ada.sync(space, folder);
        QVERIFY(SharedSpaces::join(bo.here, dir.filePath("bo.omabook-invite"), "Our Home").has_value());
        QVERIFY(SharedSpaces::sync(bo.here, space, folder)->refused.isEmpty());

        const QStringList problems = bo.sync();
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
        const std::unique_ptr<Library> there = bo.there();
        QVERIFY(SharedSpace::isShared(*there, space));
        QCOMPARE(spaceTitles(*there, space), QStringList({"Dune"}));
        // The key, kept there for its owner alone, the one here.
        const QString key = SharedSpaces::keyFile(*there, space);
        QVERIFY(key.startsWith(dir.filePath("there-home/.local/share/omabook/spaces/")));
        QCOMPARE(QFile::permissions(key) & ~(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser),
                 QFile::Permissions());
        const auto keptThere = SpaceKey::load(key);
        const auto keptHere = SpaceKey::load(SharedSpaces::keyFile(bo.here, space));
        QVERIFY(keptThere && keptHere && std::ranges::equal(keptThere->bytes(), keptHere->bytes()));
        // Filed into the same collection of one's own, its book there once.
        QCOMPARE(linkedPath(*there, space), QString("Our Home"));
        const qint64 dune = bookTitled(*there, "Dune");
        QVERIFY(dune > 0 && there->collectionNamed("Our Home")->contains(*there->book(dune)));
        QCOMPARE(std::ranges::count(there->books(), QString("Dune"), [](const Book &book) { return book.meta.title; }), 1);

        // There writes it, as the writer one is, and every member takes it.
        fileBook(*there, "Our Home", "Persuasion", "9780141439686");
        const auto wrote = SharedSpaces::sync(*there, space, folder);
        QVERIFY2(wrote && wrote->written > 0 && wrote->refused.isEmpty(), qPrintable(whyNot(wrote)));
        const SharedSpaces::Synced adas = ada.sync(space, folder);
        QVERIFY2(adas.refused.isEmpty(), qPrintable(adas.refused.join('\n')));
        QCOMPARE(spaceTitles(ada.library, space), QStringList({"Dune", "Persuasion"}));
        // And what it wrote comes back here over ssh, taken by the space's rules.
        QVERIFY(bo.sync().isEmpty());
        QCOMPARE(spaceTitles(bo.here, space), QStringList({"Dune", "Persuasion"}));
        QVERIFY(SharedSpace::state(bo.here, space).refused.isEmpty());
    }

    // #49: a key changed on one of one's computers reaches the others with
    // every generation of it, and they read and write on with the new one.
    void aKeyChangedOnOneComputerReachesTheOtherOverSsh() {
        QTemporaryDir dir;
        const QString folder = dir.filePath("Synced");
        QVERIFY(QDir().mkpath(folder));
        OwnComputers bo(dir);
        QVERIFY2(bo.sync().isEmpty(), "the first sync pairs");
        fileBook(bo.here, "Home", "Dune", "9780441172719");
        const auto created = SharedSpaces::create(bo.here, "Home", {});
        QVERIFY2(created, qPrintable(whyNot(created)));
        const QString space = created->space.space;
        Computer cy(dir, "cy");
        QVERIFY(SharedSpaces::invite(bo.here, space, cy.id(), Role::Writer, dir.filePath("cy.omabook-invite")).has_value());
        QVERIFY(SharedSpaces::sync(bo.here, space, folder).has_value());
        QVERIFY(SharedSpaces::join(cy.library, dir.filePath("cy.omabook-invite")).has_value());
        std::ignore = cy.sync(space, folder);
        QVERIFY(bo.sync().isEmpty());

        // Removed here: the key changes here, and goes there with the one before it.
        QVERIFY(SharedSpaces::remove(bo.here, space, cy.id()).has_value());
        QVERIFY(SharedSpaces::sync(bo.here, space, folder).has_value());
        const QStringList problems = bo.sync();
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
        const std::unique_ptr<Library> there = bo.there();
        const auto idsOf = [&space](const Library &library) {
            QStringList ids;
            if (const auto keys = SharedKeys::held(library, space))
                for (const SpaceKey &key : *keys)
                    ids << key.id();
            return ids;
        };
        QCOMPARE(idsOf(*there).size(), 2);
        QCOMPARE(idsOf(*there), idsOf(bo.here));
        QCOMPARE(idsOf(*there).first(), SpaceKey::load(SharedSpaces::keyFile(bo.here, space))->id());
        for (QDirIterator files(SharedKeys::folder(*there), QDir::Files); files.hasNext();)
            QCOMPARE(QFile::permissions(files.next()) & ~(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser),
                     QFile::Permissions());

        // There writes on with the new key: here reads it, Cy, removed, doesn't.
        fileBook(*there, "Home", "Persuasion", "9780141439686");
        const auto wrote = SharedSpaces::sync(*there, space, folder);
        QVERIFY2(wrote && wrote->written > 0 && wrote->refused.isEmpty(), qPrintable(whyNot(wrote)));
        QVERIFY(SharedSpaces::sync(bo.here, space, folder).has_value());
        QCOMPARE(spaceTitles(bo.here, space), QStringList({"Dune", "Persuasion"}));
        std::ignore = cy.sync(space, folder);
        QVERIFY(!spaceTitles(cy.library, space).contains("Persuasion"));
    }

    // #51: subscribing once is subscribing on all one's computers, the graph
    // cut where each is in the book; unsubscribing on one is on all.
    void aSubscriptionAndUnsubscribingTravelToOnesOtherComputers() {
        QTemporaryDir dir;
        const SpaceSnapshot out = cysCollection(dir);
        OwnComputers bo(dir);
        QVERIFY(bookOfFile(dir, bo.here, "Dune") > 0);
        QVERIFY2(bo.sync().isEmpty(), "the first sync pairs, and the book goes there");
        QVERIFY(Subscriptions::subscribe(bo.here, out, {.as = "From Cy", .dryRun = false}).has_value());
        const SpaceLinks::Link followed{.space = out.space, .transport = "folder", .address = dir.filePath("cy-out")};
        SpaceLinks::remember(bo.here, SpaceLinks::Role::Followed, followed);

        QVERIFY(bo.sync().isEmpty());
        std::unique_ptr<Library> there = bo.there();
        QVERIFY(SubscribedSpace::isSubscribed(*there, out.space));
        QCOMPARE(linkedPath(*there, out.space), QString("From Cy"));
        QVERIFY(SpaceLinks::of(*there, out.space, SpaceLinks::Role::Followed) == followed);
        // The layer's graph, there, as far as one has read there.
        const qint64 dune = bookTitled(*there, "Dune");
        QVERIFY(dune > 0);
        there->setProgress(dune, {.section = 0, .fraction = 0.5, .percent = 50});
        QCOMPARE(layerNames(*there, dune), QStringList({"Paul"}));

        // Given up there: given up here too, the collection kept.
        QVERIFY(Subscriptions::unsubscribe(*there, out.space).has_value());
        there.reset();
        QVERIFY(bo.sync().isEmpty());
        QVERIFY(!SubscribedSpace::isSubscribed(bo.here, out.space));
        QVERIFY(bo.here.collectionNamed("From Cy").has_value());
        QVERIFY(!SubscribedSpace::isSubscribed(*bo.there(), out.space));
        // Subscribing again later wins over that.
        QVERIFY(Subscriptions::subscribe(bo.here, out, {}).has_value());
        QVERIFY(bo.sync().isEmpty());
        QVERIFY(SubscribedSpace::isSubscribed(*bo.there(), out.space));
    }

    // #51: a computer that isn't one's own person gets nothing of the spaces
    // one is in; once it is, it does.
    void nothingOfOnesSpacesGoesToAComputerThatIsNotOneself() {
        QTemporaryDir dir;
        const SpaceSnapshot out = cysCollection(dir);
        const Person theirs = newPerson();
        const Person mine = keptOver(theirs.id());
        QVERIFY(mine.keepIn(dir.filePath("here-home/omabook/person.key")));
        QVERIFY(theirs.keepIn(thereKey(dir)));
        // There can't keep the person it would give up, so it stays who it is: not paired.
        const QString formers = dir.filePath("there-home/.local/share/omabook/former-persons");
        QFile blocking(formers);
        QVERIFY(blocking.open(QIODevice::WriteOnly));
        blocking.close();
        OwnComputers bo(dir);
        fileBook(bo.here, "Home", "Dune", "9780441172719");
        const auto created = SharedSpaces::create(bo.here, "Home", {});
        QVERIFY(created.has_value());
        const QString space = created->space.space;
        QVERIFY(Subscriptions::subscribe(bo.here, out, {}).has_value());

        const QStringList problems = bo.sync();
        QVERIFY2(problems.join('\n').contains("could not take on the person"), qPrintable(problems.join('\n')));
        std::unique_ptr<Library> there = bo.there();
        QCOMPARE(there->person()->id(), theirs.id());
        QVERIFY(SharedSpace::spaces(*there).isEmpty());
        QVERIFY(SharedSpace::held(*there, space).isEmpty());
        QVERIFY(QDir(dir.filePath("there-home/.local/share/omabook/spaces")).isEmpty());
        QVERIFY(SubscribedSpace::list(*there).isEmpty());
        QVERIFY(SubscribedSpace::operations(*there, out.space).isEmpty());

        // Paired, one person: the sync carries them.
        QVERIFY(QFile::remove(formers));
        QVERIFY(bo.sync().isEmpty());
        there = bo.there();
        QCOMPARE(there->person()->id(), mine.id());
        QCOMPARE(SharedSpace::spaces(*there), QStringList{space});
        QVERIFY(SpaceKey::load(SharedSpaces::keyFile(*there, space)).has_value());
        QVERIFY(SubscribedSpace::isSubscribed(*there, out.space));
    }

    // #51: only one's own person opens what is carried, and only what they
    // signed is taken.
    void whatIsCarriedOpensOnlyForOneselfAndIsTakenOnlyFromOneself() {
        QTemporaryDir dir;
        Computer bo(dir, "bo");
        bo.file("Home", "Dune", "9780441172719");
        const auto created = SharedSpaces::create(bo.library, "Home", {});
        QVERIFY(created.has_value());
        const auto key = SpaceKey::load(SharedSpaces::keyFile(bo.library, created->space.space));
        QVERIFY(key.has_value());
        const QString keyText =
            QString::fromLatin1(Crypto::toByteArray(key->bytes()).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
        const Person eve = newPerson();
        const auto asked = [&bo](const QString &person) {
            return SyncLog::answer(bo.library, {{"space", "personal:eve"},
                                                {"device", "eve"},
                                                {"vector", QJsonObject()},
                                                {"ops", QJsonArray()},
                                                {"signs", 1},
                                                {"carries", 1},
                                                {"person", person},
                                                {"carrier", person},
                                                {"carrying", true},
                                                {"dry_run", false}});
        };
        // Someone else asking is carried nothing, nor is an Omabook from before, which doesn't carry.
        QVERIFY(!asked(eve.id()).contains("carried"));
        QVERIFY(SpaceCarry::offer(bo.library, {{"signs", 1}, {"person", bo.id()}, {"carrier", bo.id()}}).isEmpty());
        // Someone saying they are Bo is, sealed: they can't open it, and the key is nowhere in it.
        const QJsonObject claimed = asked(bo.id());
        QVERIFY(claimed.contains("carried"));
        const QByteArray box = QByteArray::fromBase64(claimed.value("carried").toString().toLatin1(), QByteArray::Base64UrlEncoding);
        QVERIFY(!eve.openSealed(box));
        QVERIFY(!QJsonDocument(claimed).toJson().contains(keyText.toLatin1()));
        // Bo's other computer opens it, and takes the space.
        QVERIFY(bo.library.person()->keepIn(dir.filePath("other/home/omabook/person.key")));
        Computer other(dir, "other");
        QCOMPARE(other.id(), bo.id());
        const SpaceCarry::Taken taken = SpaceCarry::take(other.library, claimed);
        QVERIFY2(taken.problems.isEmpty(), qPrintable(taken.problems.join('\n')));
        QCOMPARE(taken.spaces, 1);
        QCOMPARE(taken.keys, 1);
        QVERIFY(SharedSpace::isShared(other.library, created->space.space));
        QVERIFY(SpaceKey::load(SharedSpaces::keyFile(other.library, created->space.space)).has_value());

        // Sealed to Bo, but signed by someone else: refused whole.
        const QJsonObject forged{
            {"shared", QJsonArray{QJsonObject{{"space", "shared:forged"}, {"name", "Forged"}, {"keys", QJsonArray()}}}}};
        const auto sealed = Person::sealFor(bo.library.person()->publicKey(), QJsonDocument(forged).toJson(QJsonDocument::Compact));
        QVERIFY(sealed.has_value());
        const Person::Signature signature = eve.sign(Crypto::bytes(*sealed));
        const SpaceCarry::Taken refused = SpaceCarry::take(
            bo.library,
            {{"carried", QString::fromLatin1(sealed->toBase64(QByteArray::Base64UrlEncoding))},
             {"carried_signature", QString::fromLatin1(Crypto::toByteArray(signature).toBase64(QByteArray::Base64UrlEncoding))}});
        QCOMPARE(refused.problems.size(), 1);
        QVERIFY(!SharedSpace::isShared(bo.library, "shared:forged"));
    }
};

QTEST_GUILESS_MAIN(PairingTests)
#include "pairing_tests.moc"
