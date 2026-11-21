// What a plugin writes beside a book being read (#137), as the notebooks
// plugin's place references need it: the reader's selection as a passage,
// passages made at a place and found again in another edition, every book
// told of by the id that is the same on every computer, whether a book goes
// into a shared or published space, and ink batches that never go over what
// one change of the log takes.

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

#include "backend.h"
#include "bookplace.h"
#include "bookspaces.h"
#include "booktext.h"
#include "filepicker.h"
#include "fixtures.h"
#include "inkcodec.h"
#include "inkstroke.h"
#include "library.h"
#include "publishedlog.h"
#include "reader.h"
#include "readingselection.h"
#include "scriptplugin.h"
#include "scriptplugins.h"
#include "settings.h"
#include "spaces.h"
#include "subscribedspace.h"

namespace {

using Fixtures::whyNot;

class NoPicker : public FilePicker {
public:
    void pick(Purpose, const QString &) override {}
};

const QList<Fixtures::Chapter> firstEdition{
    {"The Ferry", "<p>It was late in the autumn when the ferry came back to the island. The harbour master counted the crates "
                  "twice, as he always did, and wrote the numbers in a book that nobody read.</p>"},
    {"The Lantern", "<p>Mira stood at the rail and watched the lantern on the far pier go out, one wick at a time. She had "
                    "received the letter three days before, and had not opened it.</p>"},
};

// The same book with a sentence put in where the second chapter starts.
const QList<Fixtures::Chapter> secondEdition{
    firstEdition.at(0),
    {"The Lantern", "<p>The wind had turned in the night. Mira stood at the rail and watched the lantern on the far pier go "
                    "out, one wick at a time. She had received the letter three days before, and had not opened it.</p>"},
};

// A plugin that keeps what it is told, and asks what it is asked to.
const QByteArray script = R"js(
var heard = [];
omabook.on("reader.selected", function (event) { heard.push(event); });
omabook.on("book.added", function (event) { heard.push({added: event.book.id, global_id: event.book.global_id}); });
function told() { return heard; }
function selection() { return omabook.reader.selection(); }
function make(book, place) { return omabook.passages.make(book, place); }
function find(book, passage) { return omabook.passages.find(book, passage); }
function listed() { return omabook.library.list().map(function (book) { return book.global_id; }); }
function got(id) { return omabook.library.get(id).global_id; }
function byGlobal(id) { var book = omabook.library.byGlobalId(id); return book ? book.id : 0; }
function shared(id) { return omabook.library.sharedIn(id); }
function current() { return omabook.reader.current(); }
)js";

bool writePlugin(const QString &plugins) {
    const QString folder = plugins + "/beside";
    return QDir().mkpath(folder) && Fixtures::write(folder + "/plugin.json", R"({"name": "Beside", "script": "main.js"})")
           && Fixtures::write(folder + "/main.js", script);
}

QJsonValue call(ScriptPlugin &plugin, const QString &function, const QJsonArray &arguments, QString *problem = nullptr) {
    QString why;
    const QJsonValue answer = plugin.callNow(function, arguments, &why);
    if (problem)
        *problem = why;
    else if (!why.isEmpty())
        qWarning().noquote() << function << why;
    return answer;
}

// The words a passage of a book's says it holds, and those its places point to.
QString wordsAt(const BookText &text, const Passage &passage) {
    const qsizetype from = text.offsetOf(passage.from);
    return text.text(passage.from.section).mid(from, text.offsetOf(passage.to) - from);
}

} // namespace

class PluginReadingTests : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    std::unique_ptr<Library> m_library;
    qint64 m_first = 0, m_second = 0;

    ScriptServices services() {
        ScriptServices services;
        services.library = [this] { return m_library.get(); };
        return services;
    }

private slots:
    void initTestCase() {
        QVERIFY(Fixtures::chapteredEpub(m_dir.filePath("first.epub"), "The Ferry", firstEdition));
        QVERIFY(Fixtures::chapteredEpub(m_dir.filePath("second.epub"), "The Ferry, Revised", secondEdition));
        m_library = std::make_unique<Library>(m_dir.filePath("Library"));
        QVERIFY(m_library->open());
        m_first = m_library->add(m_dir.filePath("first.epub")).id;
        m_second = m_library->add(m_dir.filePath("second.epub")).id;
        QVERIFY(m_first > 0 && m_second > 0);
        QVERIFY(writePlugin(m_dir.filePath("plugins")));
    }

    // A passage at a place is the words that start there: from the start of
    // the word the place is in, twelve of them, with those around them; on a
    // page without words, the place alone.
    void aPassageStartsAtAPlace() {
        const QString text = "one two three four five six seven eight nine ten eleven twelve thirteen fourteen";
        const BookText book(2, [text](int section) { return section == 0 ? text : QString(); });
        const Passage passage = Passage::at(book, {.section = 0, .fraction = 5.0 / static_cast<double>(text.size())});
        QCOMPARE(passage.quote, QString("two three four five six seven eight nine ten eleven twelve thirteen"));
        QCOMPARE(passage.before, QString("one "));
        QCOMPARE(wordsAt(book, passage), passage.quote);
        QCOMPARE(Passage::at(book, {.section = 0, .fraction = 0}, 2).quote, QString("one two"));
        // Past the last word, and on a page of none.
        QVERIFY(Passage::at(book, {.section = 0, .fraction = 1}).quote.isEmpty());
        const Passage none = Passage::at(book, {.section = 1, .fraction = 0.5});
        QVERIFY(none.quote.isEmpty());
        QCOMPARE(none.from, (BookPlace{.section = 1, .fraction = 0.5}));
    }

    // A plugin makes a passage at a place, or of the words chosen there, and
    // finds it again in another edition, where a sentence came before it.
    void aPluginMakesPassagesAndFindsThemAgain() {
        ScriptPlugin plugin(ScriptManifest::read(m_dir.filePath("plugins/beside")), services());
        QString problem;
        QVERIFY2(plugin.load(&problem), qPrintable(problem));

        const QJsonObject atPlace = call(plugin, "make", {m_first, QJsonObject{{"section", 1}, {"fraction", 0.0}}}).toObject();
        QVERIFY2(atPlace.value("quote").toString().startsWith("Mira stood at the rail"), qPrintable(atPlace.value("quote").toString()));
        QCOMPARE(atPlace.value("from").toObject().value("section").toInt(), 1);

        const QJsonObject chosen =
            call(plugin, "make", {m_first, QJsonObject{{"section", 1}, {"fraction", 0.4}, {"quote", "the far  pier"}}}).toObject();
        QCOMPARE(chosen.value("quote").toString(), QString("the far pier"));
        QVERIFY(chosen.value("before").toString().endsWith("lantern on "));
        QVERIFY(chosen.value("after").toString().startsWith(" go out"));

        // Words the book doesn't have, and a place without a book, throw.
        std::ignore = call(plugin, "make", {m_first, QJsonObject{{"section", 1}, {"fraction", 0.4}, {"quote", "a dragon"}}}, &problem);
        QVERIFY2(problem.contains("aren't in the book"), qPrintable(problem));
        std::ignore = call(plugin, "make", {m_first, QJsonValue::Null}, &problem);
        QVERIFY(!problem.isEmpty());

        // In the second edition the words are further into their chapter, and found there.
        const QJsonObject found = call(plugin, "find", {m_second, chosen}).toObject();
        const QJsonObject again = found.value("passage").toObject();
        QCOMPARE(again.value("quote").toString(), QString("the far pier"));
        QCOMPARE(again.value("from").toObject().value("section").toInt(), 1);
        QVERIFY(again.value("from").toObject().value("fraction").toDouble() > chosen.value("from").toObject().value("fraction").toDouble());
        QVERIFY(found.value("confidence").toDouble() > 0.9);
        // A passage made of a selection is the selection's.
        const QJsonObject ofSelection = call(plugin, "make", {m_second, QJsonObject{{"book", m_second}, {"passage", again}}}).toObject();
        QCOMPARE(ofSelection.value("quote").toString(), QString("the far pier"));
        // Words that are gone are not found.
        QJsonObject gone = chosen;
        gone.insert("quote", "a dragon slept under the bridge");
        gone.insert("before", "");
        gone.insert("after", "");
        QVERIFY(call(plugin, "find", {m_second, gone}).isNull());
    }

    // Every book a plugin is told of carries its global id, the same that
    // names it in the change log, and a book is found by it.
    void everyBookIsToldOfByItsGlobalId() {
        QTemporaryDir plugins;
        QVERIFY(writePlugin(plugins.path()));
        ScriptPlugins scripts(services(), plugins.path(), nullptr, false);
        ScriptPlugin *plugin = scripts.plugin("beside");
        QVERIFY(plugin != nullptr);
        const QString first = Spaces::bookId(*m_library, m_first);
        QVERIFY(first.startsWith("b:"));
        QVERIFY(call(*plugin, "listed", {}).toArray().contains(first));
        QCOMPARE(call(*plugin, "got", {m_first}).toString(), first);
        QCOMPARE(call(*plugin, "byGlobal", {first}).toInteger(), m_first);
        QCOMPARE(call(*plugin, "byGlobal", {"b:nothing-has-this"}).toInteger(), 0);

        // A book that comes in is told of with it; and so is where the reader is.
        scripts.libraryChanged(m_library->books(), m_library->root());
        QVERIFY(Fixtures::chapteredEpub(m_dir.filePath("third.epub"), "The Pier", firstEdition.mid(1)));
        const qint64 third = m_library->add(m_dir.filePath("third.epub")).id;
        scripts.libraryChanged(m_library->books(), m_library->root());
        const QJsonArray heard = call(*plugin, "told", {}).toArray();
        QCOMPARE(heard.size(), 1);
        QCOMPARE(heard.first().toObject().value("global_id").toString(), Spaces::bookId(*m_library, third));
        scripts.emitEvent("page.turned", {{"id", m_first}, {"section", 1}, {"fraction", 0.5}});
        QCOMPARE(call(*plugin, "current", {}).toObject().value("global_id").toString(), first);
    }

    // What the reader selected, as a passage, and none once it is gone or
    // another book opens; each told as it changes, once.
    void theSelectionIsToldAsItChanges() {
        QTemporaryDir plugins;
        QVERIFY(writePlugin(plugins.path()));
        ScriptPlugins scripts(services(), plugins.path(), nullptr, false);
        ScriptPlugin *plugin = scripts.plugin("beside");
        QVERIFY(plugin != nullptr);
        QVERIFY(call(*plugin, "selection", {}).isNull());
        scripts.emitEvent("book.opened", {{"id", m_first}, {"section", 1}, {"fraction", 0.0}});
        const QJsonObject selected{{"book", m_first},
                                   {"global_id", Spaces::bookId(*m_library, m_first)},
                                   {"passage", QJsonObject{{"quote", "the far pier"}}},
                                   {"confidence", 1}};
        scripts.setSelection(selected);
        scripts.setSelection(selected);
        QCOMPARE(call(*plugin, "selection", {}).toObject(), selected);
        scripts.emitEvent("book.opened", {{"id", m_second}, {"section", 0}, {"fraction", 0.0}});
        QVERIFY(call(*plugin, "selection", {}).isNull());
        const QJsonArray heard = call(*plugin, "told", {}).toArray();
        QCOMPARE(heard.size(), 2);
        QCOMPARE(heard.at(0).toObject(), selected);
        QCOMPARE(heard.at(1).toObject().value("book").toInteger(), m_first);
        QVERIFY(heard.at(1).toObject().value("passage").isNull());
    }

    // The reader's words selected become a passage of the book's own text,
    // found nearest where the reader is, and reach the plugins.
    void theReaderSaysWhatIsSelected() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        settings.setLibraryPath(dir.filePath("Library"));
        NoPicker picker;
        QVERIFY(writePlugin(dir.filePath("plugins")));
        Backend backend(&settings, &picker, nullptr, {}, {}, dir.filePath("plugins"), "-");
        const qint64 id = backend.library()->add(m_dir.filePath("second.epub")).id;
        QVERIFY(id > 0);
        backend.scripts()->reload();
        QTRY_VERIFY(backend.scripts()->plugin("beside") != nullptr);
        Reader reader(&backend, {});
        reader.openBook(id);
        QTRY_VERIFY(reader.isOpen());
        reader.selection()->changed("the far\npier");
        ScriptPlugin *plugin = backend.scripts()->plugin("beside");
        QTRY_VERIFY(call(*plugin, "selection", {}).isObject());
        const QJsonObject selected = call(*plugin, "selection", {}).toObject();
        QCOMPARE(selected.value("book").toInteger(), id);
        QCOMPARE(selected.value("global_id").toString(), Spaces::bookId(*backend.library(), id));
        const Passage passage = Passage::fromJson(selected.value("passage").toObject());
        QCOMPARE(passage.quote, QString("the far pier"));
        QCOMPARE(passage.from.section, 1);
        QVERIFY(passage.before.endsWith("lantern on "));
        reader.selection()->changed({});
        QTRY_VERIFY(call(*plugin, "selection", {}).isNull());
        // Words that are nowhere in the book are no passage.
        QVERIFY(reader.selection()->passage("a dragon slept").isEmpty());
    }

    // A book is in a shared or published space when a collection that
    // stands for one holds it; a book a plugin keeps private is in none.
    void aBookSaysTheSpacesItGoesInto() {
        Library &library = *m_library;
        const auto club = library.addCollection("Club");
        const auto own = library.addCollection("Mine");
        QVERIFY2(club && own, qPrintable(whyNot(club)));
        QVERIFY(library.putInCollection(*club, m_first, true));
        QVERIFY(library.putInCollection(*own, m_second, true));
        QVERIFY(BookSpaces::of(library, m_first).isEmpty());

        // Published from here, and one subscribed to that is linked to another collection.
        Spaces::ensurePublishedSpace(library, "published:club", "The Club", "person-of-the-owner", "Ada");
        PublishedLog::remember(library, "published:club", {.collection = Spaces::collectionId(library, *club), .parts = {}});
        Spaces::ensurePublishedSpace(library, "published:theirs", "Theirs", "someone-else", "Bo");
        SubscribedSpace::link(library, "published:theirs", *own);
        const QList<BookSpaces::Held> held = BookSpaces::of(library, m_first);
        QCOMPARE(held.size(), 1);
        QCOMPARE(held.first().name, QString("The Club"));
        QCOMPARE(held.first().kind, QString("published"));
        QCOMPARE(held.first().collection, *club);
        QCOMPARE(BookSpaces::of(library, m_second).size(), 1);
        QCOMPARE(BookSpaces::of(library, m_second).first().space, QString("published:theirs"));

        ScriptPlugin plugin(ScriptManifest::read(m_dir.filePath("plugins/beside")), services());
        QString problem;
        QVERIFY2(plugin.load(&problem), qPrintable(problem));
        const QJsonArray spaces = call(plugin, "shared", {m_first}).toArray();
        QCOMPARE(spaces.size(), 1);
        QCOMPARE(spaces.first().toObject().value("name").toString(), QString("The Club"));

        // A notebook in the club's collection stays its person's.
        const qint64 notebook = Fixtures::privateNotebook(library, m_dir.path(), "Field Notes");
        QVERIFY(notebook > 0);
        QVERIFY(library.putInCollection(*club, notebook, true));
        QVERIFY(BookSpaces::of(library, notebook).isEmpty());
        QCOMPARE(call(plugin, "shared", {notebook}).toArray().size(), 0);
    }

    // The lasso moving a whole page of strokes is more than one change of the
    // log takes: it goes as several batches, each within the limits, those
    // that add before those that remove, so a stroke is never lost between them.
    void aLargeChangeAddsBeforeItRemoves() {
        Ink::Change moved;
        for (int i = 0; i < 1200; ++i) {
            Ink::Stroke stroke;
            stroke.id = Ink::newId();
            stroke.began = i;
            for (int p = 0; p < 20; ++p)
                stroke.points.push_back({.at = QPointF(i % 40 + p, i / 40), .pressure = 0.5F, .xTilt = 0, .yTilt = 0, .ms = 0});
            moved.added.push_back(stroke);
            moved.removed.append(Ink::newId());
        }
        const auto batches = Ink::batches(moved);
        QVERIFY(batches.size() > 2);
        bool removing = false;
        qsizetype added = 0, removed = 0;
        for (const QJsonObject &batch : batches) {
            const qsizetype adds = batch.value("add").toObject().size(), removes = batch.value("remove").toArray().size();
            QVERIFY(adds + removes <= Ink::maxBatchItems);
            QVERIFY(QJsonDocument(batch).toJson(QJsonDocument::Compact).size() < 64 * 1024);
            QVERIFY2(!(removing && adds > 0), "a batch added strokes after one that removed some");
            removing = removing || removes > 0;
            added += adds;
            removed += removes;
        }
        QCOMPARE(added, 1200);
        QCOMPARE(removed, 1200);
    }
};

QTEST_MAIN(PluginReadingTests)
#include "pluginreading_tests.moc"
