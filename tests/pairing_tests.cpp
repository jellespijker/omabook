// Signed operations in the log, and pairing a computer into its person's
// personal space (ADR 0001, *People and keys*). Every key here is made on the
// spot in a temporary folder: no real key is ever read.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <utility>

#include "fixtures.h"
#include "hlc.h"
#include "library.h"
#include "person.h"
#include "publishedspace.h"
#include "remote.h"
#include "replication.h"
#include "signedops.h"
#include "spacereplay.h"
#include "spaces.h"
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
};

QTEST_GUILESS_MAIN(PairingTests)
#include "pairing_tests.moc"
