#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QtTest>
#include <optional>

#include "assistant.h"
#include "backend.h"
#include "filepicker.h"
#include "fixtures.h"
#include "graph.h"
#include "graphbuilder.h"
#include "prompts.h"
#include "reader.h"
#include "readingmarks.h"
#include "settings.h"
#include "textmarks.h"

using TextMarks::Mark;
using TextMarks::Matcher;
using TextMarks::Thing;

namespace {

Thing thing(qint64 id, const QString &kind, const QString &name, const QStringList &aliases = {}) {
    return {id, kind, name, {}, aliases, {}};
}

// What the marks in a text say, as the words they cover.
QStringList marked(const QString &text, const QList<Mark> &marks) {
    QStringList words;
    for (const Mark &mark : marks)
        words << text.mid(mark.start, mark.length);
    return words;
}

class NoPicker : public FilePicker {
public:
    void pick(Purpose, const QString &) override {}
};

struct AiLog {
    QList<AiRequest> requests;
    QString answer = "1. Why does Ilse row at night?\n2. Who taught her the tides?\n- What is the Grey Ferry?";
};

class FakeAi : public AiProvider {
public:
    explicit FakeAi(AiLog *log) : m_log(log) {}
    QString name() const override { return "Testbot"; }
    void ask(const AiRequest &request) override {
        m_log->requests << request;
        QTimer::singleShot(0, this, [this] {
            emit delta(m_log->answer);
            emit finished();
        });
    }
    void cancel() override {}

private:
    AiLog *m_log;
};

// A made-up book of two chapters, a library with its graph, and the reader
// on it: nothing of a real book, nobody's own files.
struct Bench {
    QTemporaryDir dir;
    NoPicker picker;
    AiLog ai;
    std::optional<Settings> settings;
    std::optional<Backend> backend;
    std::optional<Reader> reader;
    std::optional<Assistant> assistant;
    qint64 book = 0;
    QHash<QString, qint64> ids;

    explicit Bench(const QByteArray &first = {}, const QByteArray &second = {}) {
        qputenv("XDG_CACHE_HOME", dir.filePath("cache").toUtf8());
        settings.emplace(dir.filePath("config.json"));
        settings->setLibraryPath(dir.filePath("Library"));
        auto makeAi = [this](QString *) -> std::unique_ptr<AiProvider> { return std::make_unique<FakeAi>(&ai); };
        backend.emplace(&*settings, &picker, nullptr, makeAi, Backend::Catalogs{}, dir.filePath("plugins"), "-");
        reader.emplace(&*backend, QString());
        assistant.emplace(&*reader, makeAi);
        Fixtures::Entries entries = Fixtures::epubEntries("Harbour of Glass");
        const auto chapter = [](const QByteArray &body) {
            return "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body>" + body + "</body></html>";
        };
        for (auto &entry : entries) {
            if (entry.first == "OEBPS/text/ch1.xhtml")
                entry.second = chapter(!first.isEmpty() ? first
                                                       : "<h1>One</h1><p>Ilse Varga rowed the <i>Grey Ferry</i> out of Glass Harbour. "
                                                         "Aldo Brenn watched from the wall.</p><p>Ilse thought of the Tide Guild.</p>");
            if (entry.first == "OEBPS/text/ch2.xhtml")
                entry.second = chapter(!second.isEmpty() ? second
                                                        : "<h1>Two</h1><p>Wick lit the lamps, and Ilse met Nerys Varga at last.</p>");
        }
        const QString file = dir.filePath("harbour.epub");
        Fixtures::zip(file, entries);
        book = backend->library()->add(file).id;
        Graph graph(*backend->library());
        const auto add = [&](const QString &kind, const QString &name, const QString &summary, int section, double fraction,
                             const QStringList &aliases = {}) {
            QJsonObject data;
            if (!aliases.isEmpty())
                data.insert("aliases", QJsonArray::fromStringList(aliases));
            ids.insert(name, graph.upsert({}, kind, name, summary, data, GraphPlace{book, section, fraction}));
        };
        add("character", "Ilse Varga", "A glassblower's daughter who pilots the night ferry.", 0, 0.0);
        add("item", "The Grey Ferry", "Ilse's boat, older than the guild.", 0, 0.0);
        add("place", "Glass Harbour", "A city built on the sea wall.", 0, 0.0);
        add("character", "Aldo Brenn", "The harbourmaster, who taught Ilse the tides.", 0, 0.0);
        add("group", "The Tide Guild", "Keeps the ferry lines, and their secrets.", 0, 0.0);
        add("thread", "Who opened the sea gate", "Someone let the Breaking happen.", 0, 0.0);
        add("character", "Wick", "A lamplighter turned informer.", 1, 0.5);
        add("character", "Nerys Varga", "Ilse's mother.", 1, 0.5);
        graph.link(ids.value("Aldo Brenn"), ids.value("Ilse Varga"), "mentor of", "He taught her the tides.", GraphPlace{book, 0, 0});
        graph.link(ids.value("Ilse Varga"), ids.value("Glass Harbour"), "lives in", {}, GraphPlace{book, 0, 0});
        graph.link(ids.value("Ilse Varga"), ids.value("Who opened the sea gate"), "seeks", {}, GraphPlace{book, 0, 0});
        backend->reload();
    }

    bool open(int section = 0, double fraction = 0) {
        reader->openBookAt(book, section, fraction);
        return QTest::qWaitFor([this] { return reader->isOpen(); }, 5000);
    }
};

} // namespace

class ReadingTests : public QObject {
    Q_OBJECT

private slots:
    void namesAreFoundAsWholeWordsAndByTheirOtherNames() {
        const Matcher matcher({thing(1, "character", "Ilse Varga", {"the Ferrywoman"}), thing(2, "character", "Pell"),
                               thing(3, "place", "Glass Harbour"), thing(4, "group", "The Tide Guild")});
        const QString text = "Ilse Varga rowed. Pellham was not Pell, nor Pell's brother. The Ferrywoman saw a Tide Guild cutter "
                             "near Glass-Harbour; the Tide Guild was asleep.";
        const QList<Mark> marks = matcher.match(text);
        QCOMPARE(marked(text, marks),
                 QStringList({"Ilse Varga", "Pell", "Pell", "The Ferrywoman", "Tide Guild", "Glass-Harbour", "the Tide Guild"}));
        QCOMPARE(marks.first().thing, 1);
        QCOMPARE(marks.at(3).thing, 1);
        // A character is also found by a part of their name.
        const QString later = "Later, Ilse slept and Varga dreamt.";
        QCOMPARE(marked(later, matcher.match(later)), QStringList({"Ilse", "Varga"}));
        // A selection names a thing whatever its case, and an article may lead.
        QCOMPARE(matcher.named(u"glass harbour"), 3);
        QCOMPARE(matcher.named(u"the Tide Guild"), 4);
        QCOMPARE(matcher.named(u"Tide"), 0);
    }

    void capitalsCountAndEverydayWordsAreGuarded() {
        const Matcher matcher({thing(1, "character", "Will"), thing(2, "character", "Hope"), thing(3, "event", "The Breaking"),
                               thing(4, "character", "Mara")});
        const QString text = "Will smiled. She will go, she said. Then Will left, and hope remained.\n"
                             "“Hope,” said Will, “is all we have.” Hope nodded. They spoke of Hope again, "
                             "and of the breaking of bread, and of the Breaking. Mara laughed.";
        const QList<Mark> marks = matcher.match(text);
        // "Will" and "Hope" are everyday words: marked only capitalised in
        // the middle of a sentence. "The Breaking" needs its capital. "Mara"
        // is no word, so a sentence may start with it, but not in lower case.
        QCOMPARE(marked(text, marks), QStringList({"Will", "Will", "Hope", "the Breaking", "Mara"}));
        QCOMPARE(marks.at(0).start, text.indexOf("Then Will") + 5);
        QCOMPARE(marks.at(2).start, text.indexOf("of Hope") + 3);
        // A word the chapter uses in lower case is an everyday word there,
        // in any language: "Roos" is a name and "roos" a rose.
        const Matcher dutch({thing(9, "character", "Roos")});
        const QString zin = "Roos lachte. Er bloeide een roos, en toen kwam Roos binnen.";
        QCOMPARE(marked(zin, dutch.match(zin)), QStringList({"Roos"}));
        QCOMPARE(dutch.match(zin).first().start, zin.lastIndexOf("Roos"));
        // Where the chapter never says it in lower case, it may start a sentence.
        const QString alone = "Roos lachte.";
        QCOMPARE(marked(alone, dutch.match(alone)), QStringList({"Roos"}));
    }

    void theLongestNameWinsAndASharedPartNamesNobody() {
        const Matcher matcher({thing(1, "character", "Ilse Varga"), thing(2, "character", "Nerys Varga"), thing(3, "place", "The Upper City"),
                               thing(4, "place", "City"), thing(5, "character", "Captain Merrow")});
        const QString text = "Nerys Varga climbed to the Upper City. Varga was tired; Nerys rested in the City. Captain Merrow "
                             "and Merrow's men, and a captain.";
        const QList<Mark> marks = matcher.match(text);
        QCOMPARE(marked(text, marks), QStringList({"Nerys Varga", "the Upper City", "Nerys", "City", "Captain Merrow", "Merrow"}));
        QCOMPARE(marks.at(1).thing, 3);
        QCOMPARE(marks.at(3).thing, 4);
        // Two things by one name: the one met first.
        const Matcher twins({thing(7, "character", "Ash"), thing(8, "place", "Ash")});
        const QString text2 = "They came to Ash.";
        QCOMPARE(twins.match(text2).value(0).thing, 7);
    }

    void aNameNeverRunsAcrossAParagraphOrIntoAnotherWord() {
        const Matcher matcher({thing(3, "place", "Glass Harbour"), thing(2, "character", "Pell")});
        const QString text = QString("Glass\nHarbour, and Glass") + QChar::ParagraphSeparator + "Harbour, Pellucid, Spell, Pell.";
        QCOMPARE(marked(text, matcher.match(text)), QStringList({"Pell"}));
    }

    void onlyWhatTheReaderHasMetIsKnown() {
        Bench bench;
        Graph graph(*bench.backend->library());
        const auto met = graph.metIn(bench.book);
        QVERIFY(met.size() >= 8);
        // In the order met: the front of the list is what is known.
        const QList<Thing> early = TextMarks::known(met, GraphPlace{bench.book, 0, 0.9});
        QStringList names;
        for (const Thing &one : early)
            names << one.name;
        QVERIFY(names.contains("Ilse Varga") && names.contains("Glass Harbour"));
        QVERIFY(!names.contains("Wick") && !names.contains("Nerys Varga"));
        // A thread is never marked in the text.
        QVERIFY(!names.contains("Who opened the sea gate"));

        QVERIFY(bench.open(1, 0.0));
        ReadingMarks *marks = bench.reader->marks();
        QTRY_VERIFY(marks->known() > 0);
        // At the start of the second chapter, Wick (met halfway through it) isn't known yet.
        const QString text = "Wick lit the lamps, and Ilse met Nerys Varga at last.";
        QCOMPARE(marked(text, marks->marksIn(1, text)), QStringList({"Ilse"}));
        QVERIFY(marks->thing(bench.ids.value("Wick")).isEmpty());
        QVERIFY(marks->named("Wick").isEmpty());
        // Read on past where they were met, and they are.
        bench.reader->setFraction(0.6);
        QTRY_COMPARE(marked(text, marks->marksIn(1, text)), QStringList({"Wick", "Ilse", "Nerys Varga"}));
    }

    void marksAreKeptUntilTheBooksGraphChangesOrTheReaderMeetsMore() {
        Bench bench;
        QVERIFY(bench.open(0, 0.5));
        ReadingMarks *marks = bench.reader->marks();
        QTRY_VERIFY(marks->known() > 0);
        QSignalSpy changed(marks, &ReadingMarks::changed);
        const QString text = "Ilse Varga rowed the Grey Ferry out of Glass Harbour.";
        QCOMPARE(marks->marksIn(0, text).size(), 3);
        QCOMPARE(marks->counts().misses, 1);
        // Coming back is a hit.
        QCOMPARE(marks->marksIn(0, text).size(), 3);
        QCOMPARE(marks->counts().hits, 1);
        // Turning pages within what is known changes nothing.
        bench.reader->setFraction(0.8);
        QCOMPARE(marks->marksIn(0, text).size(), 3);
        QCOMPARE(marks->counts().hits, 2);
        QCOMPARE(changed.count(), 0);

        // The graph changed, for another book only: read again, nothing marked anew.
        const int loads = marks->counts().loads;
        {
            Graph graph(*bench.backend->library());
            BookMeta other;
            other.title = "Another Book";
            const qint64 elsewhere = bench.backend->library()->addEntry(other, true).id;
            graph.upsert({}, "character", "Someone Else", {}, {}, GraphPlace{elsewhere, 0, 0});
        }
        marks->refresh();
        QCOMPARE(marks->counts().loads, loads + 1);
        QCOMPARE(changed.count(), 0);
        QCOMPARE(marks->marksIn(0, text).size(), 3);
        QCOMPARE(marks->counts().hits, 3);

        // Asking again with no change at all doesn't even read the graph.
        marks->refresh();
        QCOMPARE(marks->counts().loads, loads + 1);

        // This book's graph grew: marked anew.
        Graph(*bench.backend->library()).upsert({}, "item", "The Sea Wall", "It holds the sea back.", {}, GraphPlace{bench.book, 0, 0.1});
        marks->refresh();
        QCOMPARE(changed.count(), 1);
        const int misses = marks->counts().misses;
        QCOMPARE(marks->marksIn(0, text + " The Sea Wall held, though the sea wall of old had not.").size(), 4);
        QCOMPARE(marks->counts().misses, misses + 1);

        // Reading past where something was met is a change too.
        bench.reader->goTo(1);
        bench.reader->setFraction(0.7);
        QTRY_COMPARE(changed.count(), 2);
    }

    void theGraphAnswersWhoSomeoneIsWithoutTheAi() {
        Bench bench;
        QVERIFY(bench.open(0, 0.5));
        ReadingMarks *marks = bench.reader->marks();
        QTRY_VERIFY(marks->known() > 0);
        const QVariantMap ilse = marks->named("Ilse Varga");
        QCOMPARE(ilse.value("kind").toString(), QString("character"));
        QCOMPARE(ilse.value("summary").toString(), QString("A glassblower's daughter who pilots the night ferry."));
        QVERIFY(!ilse.value("relations").toList().isEmpty());
        QVERIFY(ilse.value("color").value<QColor>().isValid());
        QVERIFY(bench.ai.requests.isEmpty());

        // Its suggestions come from the graph: the thread it is in, where it
        // lives, who it is tied to; still no AI.
        const QVariantList suggestions = marks->suggestions(ilse.value("id").toLongLong());
        QStringList texts;
        for (const QVariant &one : suggestions)
            texts << one.toMap().value("text").toString();
        QCOMPARE(texts.size(), 3);
        QCOMPARE(texts.at(0), QString("What does Ilse Varga have to do with Who opened the sea gate?"));
        QCOMPARE(texts.at(1), QString("Why is Ilse Varga at Glass Harbour?"));
        QVERIFY(texts.at(2).contains("Aldo Brenn"));
        QVERIFY(bench.ai.requests.isEmpty());

        // More, when asked: one quick request with only the selection and
        // its paragraph, kept for the same thing at the same place.
        const QString key = marks->suggestionKey(ilse.value("id").toLongLong(), "Ilse Varga");
        bench.assistant->suggestMore(key, ilse.value("id").toLongLong(), "Ilse Varga", "Ilse Varga rowed the Grey Ferry.");
        QTRY_VERIFY(!bench.assistant->suggesting());
        QCOMPARE(bench.ai.requests.size(), 1);
        QCOMPARE(bench.ai.requests.first().tier, AiRequest::Quick);
        const QString sent = bench.ai.requests.first().messages.first().text;
        QVERIFY(sent.contains("Ilse Varga rowed the Grey Ferry."));
        QVERIFY(!sent.contains("Aldo Brenn watched"));
        QCOMPARE(marks->suggestions(ilse.value("id").toLongLong(), "Ilse Varga").size(), 6);
        bench.assistant->suggestMore(key, ilse.value("id").toLongLong(), "Ilse Varga", "Ilse Varga rowed the Grey Ferry.");
        QTest::qWait(50);
        QCOMPARE(bench.ai.requests.size(), 1);

        // Something the graph doesn't know is asked about with its paragraph alone.
        bench.ai.answer = "A gull, by the sound of it.";
        bench.assistant->identify("Skerrick", "Skerrick cried over the harbour.");
        QTRY_VERIFY(!bench.assistant->busy());
        QCOMPARE(bench.ai.requests.size(), 2);
        const QString asked = bench.ai.requests.last().messages.last().text;
        QVERIFY(asked.contains("Skerrick cried over the harbour."));
        QVERIFY(!asked.contains("Aldo Brenn watched"));
    }

    void theCardSaysHowFarTheGraphGotAndSendsNothing() {
        // A long first chapter, of which the graph noted only the start.
        QByteArray first = "<h1>One</h1>";
        for (int i = 0; i < 40; ++i)
            first += "<p>Ilse Varga rowed the Grey Ferry out of Glass Harbour again, and the tide turned under her.</p>";
        Bench bench(first);
        Library &library = *bench.backend->library();
        GraphBuild::setBuiltTo(library, bench.book, GraphPlace{bench.book, 0, 0.1});
        QVERIFY(bench.open(1, 0.5));
        ReadingMarks *marks = bench.reader->marks();
        QCOMPARE(marks->notedUpTo(), QString("Noted up to One"));
        // Saying so reads no further than the reader and asks the AI nothing;
        // the graph grows when a chapter is finished, as it always did.
        QVERIFY(bench.ai.requests.isEmpty());
        QVERIFY(!bench.assistant->graphBusy());
        // Caught up: nothing to say.
        GraphBuild::setBuiltTo(library, bench.book, GraphPlace{bench.book, 1, 0.5});
        QCOMPARE(marks->notedUpTo(), QString());
        QVERIFY(bench.ai.requests.isEmpty());
    }

    void suggestionsAreMadeFromRelations() {
        GraphNode ilse{1, {}, "character", "Ilse", {}, {}, 0, 0};
        GraphView view;
        view.nodes = {ilse, GraphNode{2, {}, "place", "Lowwater Stairs", {}, {}, 0, 0},
                      GraphNode{3, {}, "character", "Aldo", {}, {}, 0, 0}, GraphNode{4, {}, "character", "Tamsin", {}, {}, 0, 0}};
        view.edges = {GraphEdge{1, 1, 2, "hides at", {}, GraphPlace{9, 0, 0.2}}, GraphEdge{2, 3, 1, "rescued", {}, GraphPlace{9, 0, 0.1}},
                      GraphEdge{3, 1, 4, "sworn sister of", {}, GraphPlace{9, 0, 0.3}}};
        using Shape = TextMarks::Suggestion::Shape;
        const auto made = TextMarks::suggestions(ilse, GraphPlace{9, 0, 0.1}, view, GraphPlace{9, 2, 0.1});
        QCOMPARE(made.size(), 3);
        QCOMPARE(made.at(0).shape, Shape::Place);
        QCOMPARE(made.at(0).second, QString("Lowwater Stairs"));
        QCOMPARE(made.at(1).shape, Shape::Relation);
        QCOMPARE(made.at(1).relation, QString("sworn sister of"));
        QCOMPARE(made.at(2).shape, Shape::Between);
        // Met chapters ago, with nothing else to ask: what happened before.
        const auto alone = TextMarks::suggestions(ilse, GraphPlace{9, 0, 0.1}, GraphView{{ilse}, {}}, GraphPlace{9, 2, 0.1});
        QCOMPARE(alone.size(), 2);
        QCOMPARE(alone.at(0).shape, Shape::Before);
        QCOMPARE(alone.at(1).shape, Shape::Known);
        QCOMPARE(Prompts::questionsIn("1. Why?\n2. Who is \"Aldo\"?\n- \"Where now?\"\nNo question\n4. More?"),
                 QStringList({"Why?", "Who is \"Aldo\"?", "Where now?"}));
    }

    void anAnswersNamesAreMarkedAndLinked() {
        Bench bench;
        QVERIFY(bench.open(0, 0.5));
        ReadingMarks *marks = bench.reader->marks();
        QTRY_VERIFY(marks->known() > 0);
        const QString html = marks->linked("**Ilse Varga**: pilots the ferry.\n\n**Aldo Brenn**: her teacher.");
        QVERIFY(html.contains(QString("omabook-thing:%1").arg(bench.ids.value("Ilse Varga"))));
        QVERIFY(html.contains(QString("omabook-thing:%1").arg(bench.ids.value("Aldo Brenn"))));
        // Nobody known in it: shown as it was.
        QVERIFY(marks->linked("Nothing to see.").isEmpty());
        // Not past the place: Wick is not linked.
        QVERIFY(!marks->linked("Wick, the lamplighter.").contains("omabook-thing"));
    }

    void aLongChapterWithHundredsOfNamesIsMarkedQuickly() {
        // Three hundred made-up names and a chapter of some 300,000 characters.
        QList<Thing> things;
        const QStringList first{"Ilse", "Aldo", "Tamsin", "Odalys", "Pell", "Nerys", "Wick", "Anselm", "Merrow", "Brannoc",
                                "Corvin", "Delphine", "Edrik", "Faelan", "Gisela", "Halvard", "Isolde", "Jorund", "Kestrel", "Lisbet"};
        const QStringList last{"Varga", "Brenn", "Roe", "Quill", "Thorne", "Ashby", "Marsh", "Holloway", "Fenwick", "Grimsby",
                               "Larkin", "Moreau", "Penhallow", "Rook", "Sallow"};
        for (int i = 0; i < 300; ++i)
            things.append(thing(i + 1, i % 5 == 0 ? "place" : "character",
                                i % 5 == 0 ? QString("%1 %2 Hold").arg(last.at(i % last.size()), first.at(i % first.size()))
                                           : first.at(i % first.size()) + " " + last.at((i / first.size()) % last.size())));
        QString chapter;
        for (int i = 0; chapter.size() < 300000; ++i)
            chapter += QString("%1 walked the long road to %2, where the will of the guild held. ")
                           .arg(things.at(i % things.size()).name, things.at((i * 7) % things.size()).name)
                       + (i % 6 == 5 ? "\n" : "");
        QElapsedTimer timer;
        timer.start();
        const Matcher matcher(things);
        const qint64 built = timer.nsecsElapsed();
        timer.restart();
        const QList<Mark> marks = matcher.match(chapter);
        const qint64 matched = timer.nsecsElapsed();
        qInfo("%lld terms from %lld things in %.2f ms; %lld marks in %lld characters in %.2f ms", qlonglong(matcher.termCount()),
              qlonglong(things.size()), built / 1e6, qlonglong(marks.size()), qlonglong(chapter.size()), matched / 1e6);
        QVERIFY(marks.size() > 4000);
        // Generous, so a busy machine doesn't fail it; it runs off the
        // window's thread anyway.
        QVERIFY2(matched < 1500'000'000, qPrintable(QString::number(matched / 1e6)));
    }
};

QTEST_MAIN(ReadingTests)
#include "reading_tests.moc"
