#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <array>
#include <functional>
#include <latch>
#include <sys/stat.h>
#include <thread>
#include <vector>

#include "fixtures.h"
#include "library.h"
#include "person.h"
#include "scriptplugins.h"
#include "settings.h"
#include "signedops.h"
#include "spacekey.h"

// People and keys (ADR 0001): a person's key pair and the file it is kept
// in, operations signed in their canonical bytes, and batches of them
// sealed with a space's key. Everything here is made on the spot in a
// temporary folder: no real key is ever read.
class IdentityTests : public QObject {
    Q_OBJECT

    [[nodiscard]] static Person newPerson() {
        auto person = Person::generate();
        if (!person)
            qFatal("%s", qPrintable(person.error()));
        return std::move(*person);
    }

    [[nodiscard]] static SpaceKey newSpaceKey() {
        auto key = SpaceKey::generate();
        if (!key)
            qFatal("%s", qPrintable(key.error()));
        return std::move(*key);
    }

    [[nodiscard]] static SpaceOperation operation(qint64 seq = 1) {
        return {.space = QStringLiteral("space-1"),
                .device = QStringLiteral("device-a"),
                .seq = seq,
                .hlc = (qint64{1'700'000'000} << 16) + seq,
                .author = QStringLiteral("device-a"),
                .entity = QStringLiteral("book"),
                .id = QStringLiteral("book-gid"),
                .field = QStringLiteral("progress"),
                .value = QStringLiteral(R"({"section": 3, "fraction": 0.5, "percent": 40})")};
    }

    [[nodiscard]] static QList<SignedOperation> signedOps(const Person &author, int count) {
        QList<SignedOperation> ops;
        for (int seq = 1; seq <= count; ++seq)
            ops << SignedOps::sign(operation(seq), author);
        return ops;
    }

private slots:
    void signsAndVerifies() {
        const Person person = newPerson();
        const QByteArray message("what was written");
        const Person::Signature signature = person.sign(Crypto::bytes(message));
        QVERIFY(Person::verify(person.publicKey(), Crypto::bytes(message), signature));
        QVERIFY(!Person::verify(person.publicKey(), Crypto::bytes(QByteArray("what was not written")), signature));
        QVERIFY(!Person::verify(newPerson().publicKey(), Crypto::bytes(message), signature));
    }

    void anIdNamesItsPublicKey() {
        const Person person = newPerson();
        QCOMPARE(person.id().size(), 43);
        QCOMPARE(person.exportPublicKey(), person.id());
        const auto key = Person::publicKeyFrom(person.id());
        QVERIFY(key);
        QVERIFY(*key == person.publicKey());
        QVERIFY(!Person::publicKeyFrom(u"not a key"));
        QVERIFY(!Person::publicKeyFrom(person.id().left(40)));
        QVERIFY(QRegularExpression(QStringLiteral("^([0-9a-f]{4} ){7}[0-9a-f]{4}$")).match(person.fingerprint()).hasMatch());
        QCOMPARE(Person::fingerprintOf(*key), person.fingerprint());
        QVERIFY(newPerson().fingerprint() != person.fingerprint());
    }

    void canonicalBytesAreOneFormatOnly() {
        const SpaceOperation op = operation();
        const QByteArray bytes = SignedOps::canonicalBytes(op);
        // The name, the version, then the space as its length and its bytes.
        QVERIFY(bytes.startsWith(QByteArray("omabook-op\x01", 11)));
        QCOMPARE(qFromBigEndian<quint32>(bytes.mid(11, 4).constData()), quint32{7});
        QCOMPARE(bytes.mid(15, 7), QByteArray("space-1"));
        QCOMPARE(SignedOps::canonicalBytes(op), bytes);
        // The same JSON written in another order, or spaced otherwise, is the same operation.
        SpaceOperation reordered = op;
        reordered.value = QStringLiteral(R"({"percent":40,"fraction":0.5,"section":3})");
        QCOMPARE(SignedOps::canonicalBytes(reordered), bytes);
        // Any field that differs makes other bytes.
        SpaceOperation other = op;
        other.field = QStringLiteral("progres");
        other.id = QStringLiteral("sbook-gid");
        QVERIFY(SignedOps::canonicalBytes(other) != bytes);
        const auto back = SignedOps::fromCanonicalBytes(bytes);
        QVERIFY(back);
        QCOMPARE(SignedOps::canonicalBytes(*back), bytes);
        QCOMPARE(back->hlc, op.hlc);
        QCOMPARE(back->device, op.device);
        QVERIFY(!SignedOps::fromCanonicalBytes(bytes.chopped(1)));
        QVERIFY(!SignedOps::fromCanonicalBytes(bytes + 'x'));
        QByteArray newer = bytes;
        newer[10] = 2;
        QVERIFY(!SignedOps::fromCanonicalBytes(newer));
    }

    void aTamperedOperationIsRefused() {
        const Person author = newPerson();
        const SignedOperation signedOp = SignedOps::sign(operation(), author);
        QVERIFY(SignedOps::verify(signedOp));
        const auto changed = [&signedOp](const std::function<void(SpaceOperation &)> &change) {
            SignedOperation tampered = signedOp;
            change(tampered.op);
            return SignedOps::verify(tampered);
        };
        QVERIFY(!changed([](SpaceOperation &op) { op.value = QStringLiteral(R"({"section": 9})"); }));
        QVERIFY(!changed([](SpaceOperation &op) { ++op.hlc; }));
        QVERIFY(!changed([](SpaceOperation &op) { ++op.seq; }));
        QVERIFY(!changed([](SpaceOperation &op) { op.device = QStringLiteral("device-b"); }));
        QVERIFY(!changed([](SpaceOperation &op) { op.space = QStringLiteral("space-2"); }));
        QVERIFY(!changed([](SpaceOperation &op) { op.entity = QStringLiteral("collection"); }));
        // Someone else's key in place of the author's.
        SignedOperation claimed = signedOp;
        claimed.signer = newPerson().publicKey();
        QVERIFY(!SignedOps::verify(claimed));
    }

    void aBatchSealsAndOpens() {
        const Person author = newPerson();
        const SpaceKey key = newSpaceKey();
        const QList<SignedOperation> ops = signedOps(author, 3);
        const QByteArray batch = SignedOps::seal(key, u"space-1", ops);
        QVERIFY(!batch.contains("book-gid"));
        const auto opened = SignedOps::open(key, u"space-1", batch);
        QVERIFY2(opened, qPrintable(opened.error()));
        QCOMPARE(opened->size(), 3);
        for (qsizetype at = 0; at < ops.size(); ++at) {
            QCOMPARE(SignedOps::canonicalBytes(opened->at(at).op), SignedOps::canonicalBytes(ops.at(at).op));
            QVERIFY(opened->at(at).signer == author.publicKey());
        }
        const auto empty = SignedOps::open(key, u"space-1", SignedOps::seal(key, u"space-1", {}));
        QVERIFY(empty);
        QVERIFY(empty->isEmpty());
        // The key, given to another member as bytes, opens it as well.
        const auto given = SpaceKey::fromBytes(key.bytes());
        QVERIFY(given);
        QVERIFY(SignedOps::open(*given, u"space-1", batch));
        QVERIFY(!SpaceKey::fromBytes(key.bytes().first(16)));
    }

    void aBatchOpensOnlyWithItsKeyAndSpace() {
        const QByteArray batch = SignedOps::seal(newSpaceKey(), u"space-1", signedOps(newPerson(), 2));
        QVERIFY(!SignedOps::open(newSpaceKey(), u"space-1", batch));
        const SpaceKey key = newSpaceKey();
        const QByteArray sealed = SignedOps::seal(key, u"space-1", signedOps(newPerson(), 2));
        QVERIFY(SignedOps::open(key, u"space-1", sealed));
        QVERIFY(!SignedOps::open(key, u"space-2", sealed));
    }

    void aChangedOrTruncatedBatchIsRefused() {
        const SpaceKey key = newSpaceKey();
        const QByteArray batch = SignedOps::seal(key, u"space-1", signedOps(newPerson(), 3));
        QVERIFY(SignedOps::open(key, u"space-1", batch));
        // Cut anywhere: the last part (which says the batch is whole) is missing.
        for (const qsizetype cut : {qsizetype{1}, qsizetype{17}, qsizetype{21}, batch.size() / 2, batch.size() - 8})
            QVERIFY2(!SignedOps::open(key, u"space-1", batch.chopped(cut)), qPrintable(QString::number(cut)));
        QVERIFY(!SignedOps::open(key, u"space-1", batch + QByteArray(4, '\0')));
        for (qsizetype at = 0; at < batch.size(); at += 37) {
            QByteArray flipped = batch;
            flipped[at] = static_cast<char>(flipped.at(at) ^ 0x01);
            QVERIFY2(!SignedOps::open(key, u"space-1", flipped), qPrintable(QString::number(at)));
        }
        QVERIFY(!SignedOps::open(key, u"space-1", QByteArray("not a batch")));
    }

    void aBatchWithATamperedOperationIsRefused() {
        const SpaceKey key = newSpaceKey();
        QList<SignedOperation> ops = signedOps(newPerson(), 2);
        ops[1].op.value = QStringLiteral(R"({"section": 99})");
        const auto opened = SignedOps::open(key, u"space-1", SignedOps::seal(key, u"space-1", ops));
        QVERIFY(!opened);
        QVERIFY(opened.error().contains(QStringLiteral("signed")));
        // Signed rightly, but of another space than the batch says.
        const QList<SignedOperation> elsewhere = signedOps(newPerson(), 1);
        const QByteArray sealed = SignedOps::seal(key, u"space-2", elsewhere);
        QVERIFY(!SignedOps::open(key, u"space-2", sealed));
    }

    void theKeyFileIsTheOwnersOnly() {
        const QTemporaryDir folder;
        QVERIFY(folder.isValid());
        const QString file = Person::keyFile(folder.path() + QStringLiteral("/Library"));
        QCOMPARE(QFileInfo(file).fileName(), QStringLiteral("omabook-person.key"));
        // However open the umask, the key is made for its owner alone.
        const mode_t before = ::umask(0);
        const auto person = Person::loadOrCreate(file);
        ::umask(before);
        QVERIFY2(person, qPrintable(person ? QString() : person.error()));
        struct stat info{};
        QCOMPARE(::stat(QFile::encodeName(file).constData(), &info), 0);
        QCOMPARE(info.st_mode & 0777U, 0600U);
        QCOMPARE(QFileInfo(file).size(), 40);
        // No temporary file is left beside it.
        QCOMPARE(QDir(QFileInfo(file).absolutePath()).entryList(QDir::Files | QDir::Hidden).size(), 1);
        QVERIFY(QFile::setPermissions(file, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther));
        const auto shown = Person::load(file);
        QVERIFY(!shown);
        QVERIFY(shown.error().contains(QStringLiteral("chmod 600")));
        QVERIFY(!Person::loadOrCreate(file));
    }

    void aSecondStartReusesTheKey() {
        const QTemporaryDir folder;
        const QString file = Person::keyFile(folder.path());
        const auto first = Person::loadOrCreate(file);
        QVERIFY(first);
        const auto second = Person::loadOrCreate(file);
        QVERIFY(second);
        QCOMPARE(second->id(), first->id());
        QCOMPARE(second->fingerprint(), first->fingerprint());
        // The secret half is the same too: what one signs, the other's public key verifies.
        const QByteArray message("signed on the first start");
        QVERIFY(Person::verify(first->publicKey(), Crypto::bytes(message), second->sign(Crypto::bytes(message))));
        const auto loaded = Person::load(file);
        QVERIFY(loaded);
        QCOMPARE(loaded->id(), first->id());
    }

    // Two programs that start at once, before either has a key, end up with
    // one: the one kept first, which the other reads back in place of its own.
    void manyStartingAtOnceKeepOneKey() {
        constexpr int starters = 6;
        for (int round = 0; round < 20; ++round) {
            const QTemporaryDir folder;
            const QString file = Person::keyFile(folder.path());
            std::latch start{starters};
            std::array<QString, starters> ids;
            {
                std::vector<std::jthread> threads;
                for (QString &id : ids)
                    threads.emplace_back([&start, &file, &id] {
                        start.arrive_and_wait();
                        const auto person = Person::loadOrCreate(file);
                        id = person ? person->id() : QStringLiteral("failed: ") + person.error();
                    });
            }
            const auto loaded = Person::load(file);
            QVERIFY(loaded);
            for (const QString &id : ids)
                QCOMPARE(id, loaded->id());
            // Only the key is left: no other's temporary file beside it.
            QCOMPARE(QDir(folder.path()).entryList(QDir::Files | QDir::Hidden).size(), 1);
        }
    }

    void aFileThatIsNotAKeyIsRefused() {
        const QTemporaryDir folder;
        const QString file = Person::keyFile(folder.path());
        QFile written(file);
        QVERIFY(written.open(QIODevice::WriteOnly));
        QVERIFY(written.write(QByteArray(40, 'x')) == 40);
        written.close();
        QVERIFY(written.setPermissions(QFile::ReadOwner | QFile::WriteOwner));
        QVERIFY(!Person::load(file));
        QVERIFY(written.resize(12));
        QVERIFY(!Person::load(file));
        QVERIFY(!Person::load(folder.path() + QStringLiteral("/nothing.key")));
    }

    // A test program runs in a home of its own, made in the temporary folder
    // before main (Fixtures::home): the key a library signs with, the
    // settings, the plugins and the caches are all kept there, so running a
    // test never writes into the user's own home (#53).
    void theTestsKeepTheKeyAndTheSettingsInAHomeOfTheirOwn() {
        const QString &home = Fixtures::home();
        QVERIFY2(home.startsWith(QDir::cleanPath(QDir::tempPath()) + u'/'), qPrintable(home));
        QCOMPARE(QDir::homePath(), home);
        const auto inHome = [&home](const QString &path) { return path.startsWith(home + u'/'); };
        QVERIFY2(inHome(Settings::defaultFile()), qPrintable(Settings::defaultFile()));
        QVERIFY2(inHome(ScriptPlugins::defaultFolder()), qPrintable(ScriptPlugins::defaultFolder()));
        for (const auto location : {QStandardPaths::GenericDataLocation, QStandardPaths::GenericConfigLocation,
                                    QStandardPaths::GenericCacheLocation, QStandardPaths::AppDataLocation}) {
            const QString folder = QStandardPaths::writableLocation(location);
            QVERIFY2(inHome(folder), qPrintable(folder));
        }
        QVERIFY(inHome(qEnvironmentVariable("XDG_STATE_HOME")));

        const QTemporaryDir folder;
        Library library(folder.filePath(QStringLiteral("Library")));
        const QString key = library.personFile().file();
        QCOMPARE(key, home + QStringLiteral("/.local/share/omabook/person.key"));
        QVERIFY(library.personFile().get(library.root()) != nullptr);
        QVERIFY(QFile::exists(key));
    }
};

QTEST_GUILESS_MAIN(IdentityTests)
#include "identity_tests.moc"
