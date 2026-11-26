// Many adding to one library at once (#228): commands run together (as a file
// manager sends them), the MCP server, and threads of one program (the window
// beside a watched folder). The same file ends up as one book, which the others
// are told is there already; different files are all kept.

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QtTest>
#include <latch>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include "addinghold.h"
#include "importer.h"
#include "library.h"
#include "mcpharness.h"

using namespace std::chrono_literals;

namespace {

using McpHarness::Machine;

// An EPUB big enough that bringing it in takes a while: adds started together
// are then all at it at once.
[[nodiscard]] bool largeEpub(const QString &path, const QString &title) {
    QByteArray padding(48 * 1024 * 1024, Qt::Uninitialized);
    QRandomGenerator random(228);
    for (char &byte : padding)
        byte = static_cast<char>(random.generate());
    Fixtures::Entries entries = Fixtures::epubEntries(title);
    entries.append({"OEBPS/padding.bin", padding});
    return Fixtures::zip(path, entries);
}

[[nodiscard]] Machine::Ran finished(QProcess &process) {
    if (!process.waitForFinished(120000))
        return {};
    return {.code = process.exitCode(),
            .output = QString::fromUtf8(process.readAllStandardOutput()),
            .errors = QString::fromUtf8(process.readAllStandardError())};
}

// A library of the test's own, and files to add to it.
struct Shelf {
    Machine machine;

    [[nodiscard]] QString path(const QString &name) const { return machine.dir.filePath(name); }
    [[nodiscard]] bool prepared() const { return Library(machine.library).open(); }

    [[nodiscard]] std::unique_ptr<QProcess> started(const QStringList &add) const {
        auto process = std::make_unique<QProcess>();
        process->setProcessEnvironment(machine.environment());
        process->start(OMABOOK_BINARY, QStringList{"add", "--library", machine.library, "--json"} + add);
        process->closeWriteChannel();
        return process;
    }

    // `omabook add` once for each of these, all started before any is waited for.
    [[nodiscard]] QList<Machine::Ran> addAtOnce(const QList<QStringList> &adds) const {
        std::vector<std::unique_ptr<QProcess>> running;
        for (const QStringList &add : adds)
            running.push_back(started(add));
        QList<Machine::Ran> ran;
        for (const auto &process : running)
            ran.append(finished(*process));
        return ran;
    }

    [[nodiscard]] QList<Book> books() const {
        Library library(machine.library);
        return library.open() ? library.books() : QList<Book>();
    }
};

// How many of the books these runs named were new, and how many there already.
struct Said {
    int added = 0, duplicates = 0, failed = 0;
    std::set<qint64> ids;
};

[[nodiscard]] Said saidBy(const QList<Machine::Ran> &runs) {
    Said said;
    for (const Machine::Ran &ran : runs) {
        if (ran.code != 0)
            ++said.failed;
        for (const QJsonValue &item : QJsonDocument::fromJson(ran.output.toUtf8()).array()) {
            ++(item.toObject().value("duplicate").toBool() ? said.duplicates : said.added);
            said.ids.insert(item.toObject().value("id").toInteger());
        }
    }
    return said;
}

[[nodiscard]] QString errorsOf(const QList<Machine::Ran> &runs) {
    QStringList errors;
    for (const Machine::Ran &ran : runs)
        errors << ran.errors;
    return errors.join('\n');
}

[[nodiscard]] QList<QStringList> times(int count, const QStringList &add) { return QList<QStringList>(count, add); }

} // namespace

class ParallelAddsTests : public QObject {
    Q_OBJECT

private slots:
    void oneFileAddedAtOnceIsOneBook_data() {
        QTest::addColumn<bool>("described");
        QTest::newRow("as it is") << false;
        // The other way in: described by whoever adds it.
        QTest::newRow("with its details") << true;
    }
    void oneFileAddedAtOnceIsOneBook() {
        QFETCH(bool, described);
        const Shelf shelf;
        QVERIFY(shelf.prepared());
        QVERIFY(largeEpub(shelf.path("huge.epub"), "The Enormous Chronicle"));
        QStringList add{shelf.path("huge.epub")};
        if (described) {
            QVERIFY(Fixtures::write(shelf.path("meta.json"), R"({"title": "The Enormous Chronicle"})"));
            add << "--meta" << shelf.path("meta.json");
        }
        const QList<Machine::Ran> runs = shelf.addAtOnce(times(4, add));
        const Said said = saidBy(runs);
        QVERIFY2(said.failed == 0, qPrintable(errorsOf(runs)));
        QCOMPARE(shelf.books().size(), 1);
        QCOMPARE(said.added, 1);
        QCOMPARE(said.duplicates, 3);
        QCOMPARE(said.ids.size(), 1U);
        QVERIFY(QDir(shelf.machine.library).entryList({".adding-*"}, QDir::Files | QDir::Hidden).isEmpty());
    }

    void aFolderAddedAtOnceBringsEachBookOnce() {
        const Shelf shelf;
        QVERIFY(shelf.prepared());
        QVERIFY(QDir().mkpath(shelf.path("folder")));
        for (int book = 0; book < 24; ++book)
            QVERIFY(Fixtures::epub(shelf.path(QString("folder/%1.epub").arg(book)), QString("Volume %1").arg(book)));
        const QList<Machine::Ran> runs = shelf.addAtOnce(times(6, {shelf.path("folder")}));
        const Said said = saidBy(runs);
        QVERIFY2(said.failed == 0, qPrintable(errorsOf(runs)));
        QCOMPARE(shelf.books().size(), 24);
        QCOMPARE(said.added, 24);
        QCOMPARE(said.duplicates, 5 * 24);
    }

    void aBookHalfBroughtInTakesNoOtherFile() {
        const Shelf shelf;
        QVERIFY(shelf.prepared());
        // Two editions under one title and author: the second comes while the
        // first is a book still without its file, and mustn't take it for one
        // that has none.
        QVERIFY(largeEpub(shelf.path("first.epub"), "Twins"));
        QVERIFY(Fixtures::epub(shelf.path("second.epub"), "Twins"));
        const auto first = shelf.started({shelf.path("first.epub")});
        Library library(shelf.machine.library);
        QVERIFY(library.open());
        QTRY_VERIFY_WITH_TIMEOUT(!library.books().isEmpty(), 60000);
        const QList<Machine::Ran> runs{finished(*shelf.started({shelf.path("second.epub")})), finished(*first)};
        const Said said = saidBy(runs);
        QVERIFY2(said.failed == 0, qPrintable(errorsOf(runs)));
        QCOMPARE(said.added, 2);
        QCOMPARE(library.books().size(), 2);
        QVERIFY(library.bookWithHash(Library::hashFile(shelf.path("first.epub"))) > 0);
        QVERIFY(library.bookWithHash(Library::hashFile(shelf.path("second.epub"))) > 0);
    }

    void twoFilesForOneEntryAtOnceAreBothKept() {
        const Shelf shelf;
        {
            Library library(shelf.machine.library);
            QVERIFY(library.open());
            BookMeta wished;
            wished.title = "The Wished Book";
            wished.authors = {"Ann Author", "Bo Writer"}; // as the files say
            QVERIFY(library.addEntry(wished, true).id > 0);
        }
        QVERIFY(largeEpub(shelf.path("one.epub"), "The Wished Book"));
        QVERIFY(QFile::copy(shelf.path("one.epub"), shelf.path("two.epub")));
        QFile two(shelf.path("two.epub"));
        QVERIFY(two.open(QIODevice::Append) && two.write("another") > 0);
        two.close();
        const QList<Machine::Ran> runs = shelf.addAtOnce({{shelf.path("one.epub")}, {shelf.path("two.epub")}});
        QVERIFY2(saidBy(runs).failed == 0, qPrintable(errorsOf(runs)));
        Library library(shelf.machine.library);
        QVERIFY(library.open());
        // One joins the entry, the other is a book of its own.
        QCOMPARE(library.books().size(), 2);
        for (const QString &name : {"one.epub", "two.epub"})
            QVERIFY2(library.bookWithHash(Library::hashFile(shelf.path(name))) > 0, qPrintable(name));
        for (const Book &book : library.books())
            for (const FormatFile &file : library.formatFiles(book.id))
                QCOMPARE(Library::hashFile(file.path), file.hash);
    }

    void theMcpServerAndACommandAtOnceMakeOneBook() {
        const Shelf shelf;
        QVERIFY(shelf.prepared());
        QVERIFY(largeEpub(shelf.path("huge.epub"), "The Enormous Chronicle"));
        McpHarness::Server server(shelf.machine);
        QVERIFY(!server.request("initialize", {{"protocolVersion", "2025-06-18"}}).isEmpty());
        server.send(
            QJsonDocument(QJsonObject{{"jsonrpc", "2.0"},
                                      {"id", 100},
                                      {"method", "tools/call"},
                                      {"params", QJsonObject{{"name", "book_add"},
                                                             {"arguments", QJsonObject{{"paths", QJsonArray{shelf.path("huge.epub")}}}}}}})
                .toJson(QJsonDocument::Compact));
        const QList<Machine::Ran> runs = shelf.addAtOnce(times(3, {shelf.path("huge.epub")}));
        const QJsonObject answer = server.receive(120000);
        QVERIFY2(saidBy(runs).failed == 0, qPrintable(errorsOf(runs)));
        QVERIFY(!answer.value("result").toObject().value("isError").toBool());
        QCOMPARE(shelf.books().size(), 1);
    }

    void threadsOfOneProgramMakeOneBook() {
        const Shelf shelf;
        QVERIFY(shelf.prepared());
        QVERIFY(largeEpub(shelf.path("huge.epub"), "The Enormous Chronicle"));
        constexpr int writers = 3;
        std::latch ready(writers);
        std::vector<ImportSummary> summaries(writers);
        {
            std::vector<std::jthread> threads;
            for (ImportSummary &summary : summaries)
                threads.emplace_back([&shelf, &ready, &summary] {
                    Library library(shelf.machine.library);
                    const bool open = library.open();
                    ready.arrive_and_wait();
                    if (open)
                        summary = importBooks(library, {shelf.path("huge.epub")});
                });
        }
        int added = 0, duplicates = 0;
        for (const ImportSummary &summary : summaries) {
            QCOMPARE(summary.problems, QStringList());
            added += summary.added;
            duplicates += summary.duplicates;
        }
        QCOMPARE(shelf.books().size(), 1);
        QCOMPARE(added, 1);
        QCOMPARE(duplicates, writers - 1);
    }

    void addingAFormatWaitsForWhoeverHoldsTheFile() {
        const Shelf shelf;
        QVERIFY(shelf.prepared());
        QVERIFY(Fixtures::epub(shelf.path("book.epub"), "Held"));
        BookMeta meta;
        meta.title = "Elsewhere";
        Library library(shelf.machine.library);
        QVERIFY(library.open());
        const qint64 id = library.addEntry(meta, true).id;
        QVERIFY(id > 0);
        auto held = std::make_optional(AddingHold::files(shelf.machine.library, {Library::hashFile(shelf.path("book.epub"))}));
        QVERIFY(held->has_value());
        const auto adding = shelf.started({"--to", QString::number(id), shelf.path("book.epub")});
        QVERIFY(!adding->waitForFinished(2000));
        held.reset();
        const Machine::Ran ran = finished(*adding);
        QVERIFY2(ran.code == 0, qPrintable(ran.errors));
        // Added again as a book of its own, it is the one it joined.
        const Said again = saidBy(shelf.addAtOnce({{shelf.path("book.epub")}}));
        QCOMPARE(again.duplicates, 1);
        QCOMPARE(again.ids, std::set<qint64>{id});
    }

    void aHoldLeftByAProgramThatDiedIsTakenOver() {
        const Shelf shelf;
        QVERIFY(shelf.prepared());
        QVERIFY(largeEpub(shelf.path("huge.epub"), "The Enormous Chronicle"));
        const auto killed = shelf.started({shelf.path("huge.epub")});
        QTRY_VERIFY_WITH_TIMEOUT(!QDir(shelf.machine.library).entryList({".adding-*"}, QDir::Files | QDir::Hidden).isEmpty(), 60000);
        killed->kill();
        QVERIFY(killed->waitForFinished(30000));
        const QList<Machine::Ran> runs = shelf.addAtOnce({{shelf.path("huge.epub")}});
        QVERIFY2(saidBy(runs).failed == 0, qPrintable(errorsOf(runs)));
        Library library(shelf.machine.library);
        QVERIFY(library.open());
        QVERIFY(library.bookWithHash(Library::hashFile(shelf.path("huge.epub"))) > 0);
    }

    void aHeldFileIsWaitedForAndOthersAreNot() {
        const Shelf shelf;
        QVERIFY(shelf.prepared());
        const QString root = shelf.machine.library;
        const auto held = AddingHold::files(root, {"b", "a", "a", ""});
        QVERIFY(held.has_value());
        // The thread that holds it takes it again at once.
        QVERIFY(AddingHold::files(root, {"a"}, 0ms).has_value());
        bool same = true, other = false;
        std::jthread([&root, &same, &other] {
            same = AddingHold::files(root, {"a"}, 50ms).has_value();
            other = AddingHold::files(root, {"c"}, 50ms).has_value();
        }).join();
        QVERIFY(!same);
        QVERIFY(other);
    }

    void aHoldIsLetGoWithItsLastHolder() {
        const Shelf shelf;
        QVERIFY(shelf.prepared());
        const QString root = shelf.machine.library;
        QVERIFY(AddingHold::book(root, 1).has_value());
        bool taken = false;
        std::jthread([&root, &taken] { taken = AddingHold::book(root, 1, 0ms).has_value(); }).join();
        QVERIFY(taken);
    }
};

QTEST_MAIN(ParallelAddsTests)
#include "paralleladds_tests.moc"
