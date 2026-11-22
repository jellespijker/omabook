// Commands said while reading (#150): the grammar of the five languages
// over the command table, from transcripts as speech to text writes them;
// numbers in figures and in words; a chapter found by its title or its
// place; and the model asked only for what the grammar doesn't know, through
// the registry, hosted only where allowed for quick work, none with the AI
// Off, and sent nothing but the words said.

#include <QFile>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <algorithm>
#include <memory>

#include "ai.h"
#include "airegistry.h"
#include "commandtable.h"
#include "fixtures.h"
#include "settings.h"
#include "voicecommands.h"
#include "voicegrammar.h"

using namespace Qt::StringLiterals;

namespace {

// A model that answers every request the same, and keeps the last it was sent.
class CommandAi : public AiProvider {
public:
    CommandAi(QString answer, AiRequest *sent) : m_answer(std::move(answer)), m_sent(sent) {}
    QString name() const override { return u"Testbot"_s; }
    void ask(const AiRequest &request) override {
        *m_sent = request;
        QTimer::singleShot(10, this, [this] {
            emit delta(m_answer);
            emit finished();
        });
    }
    void cancel() override {}

private:
    QString m_answer;
    AiRequest *m_sent;
};

struct Said {
    const char *language;
    QString said;
    const char *command;
    int number = 0;
    QString words = {};
};

// The six of the issue, and more, as each language says them, written as a
// transcript comes: capitals, punctuation, accents (or none), a "please".
const QList<Said> &transcripts() {
    static const QList<Said> all{
        {"en", u"Next chapter."_s, "reader.next"},
        {"en", u"Previous page"_s, "reader.page_up"},
        {"en", u"Highlight this."_s, "spoken.highlight"},
        {"en", u"Read aloud, please."_s, "reader.read_aloud"},
        {"en", u"Go to chapter 5."_s, "spoken.chapter", 5},
        {"en", u"Go to chapter twenty-one"_s, "spoken.chapter", 21},
        {"en", u"What does this word mean?"_s, "spoken.meaning"},
        {"en", u"What does serendipity mean?"_s, "spoken.meaning", 0, u"serendipity"_s},
        {"en", u"Turn the page"_s, "reader.page_down"},
        {"en", u"Stop reading"_s, "reader.read_aloud_stop"},
        {"en", u"Bookmark this page"_s, "reader.bookmark"},
        {"en", u"Omabook, make the text bigger"_s, "reader.larger"},
        {"de", u"Nächstes Kapitel"_s, "reader.next"},
        {"de", u"Vorherige Seite."_s, "reader.page_up"},
        {"de", u"Markiere das"_s, "spoken.highlight"},
        {"de", u"Vorlesen, bitte"_s, "reader.read_aloud"},
        {"de", u"Lies mir vor"_s, "reader.read_aloud"},
        {"de", u"Gehe zu Kapitel fünf"_s, "spoken.chapter", 5},
        {"de", u"Kapitel einundzwanzig"_s, "spoken.chapter", 21},
        {"de", u"Was bedeutet dieses Wort?"_s, "spoken.meaning"},
        {"de", u"Was bedeutet Fernweh?"_s, "spoken.meaning", 0, u"Fernweh"_s},
        {"de", u"Umblättern"_s, "reader.page_down"},
        {"de", u"Schrift größer"_s, "reader.larger"},
        {"fr", u"Chapitre suivant"_s, "reader.next"},
        {"fr", u"Page précédente"_s, "reader.page_up"},
        {"fr", u"Surligne ça"_s, "spoken.highlight"},
        {"fr", u"Lis à voix haute, s'il te plaît"_s, "reader.read_aloud"},
        {"fr", u"Va au chapitre cinq"_s, "spoken.chapter", 5},
        {"fr", u"Chapitre vingt et un"_s, "spoken.chapter", 21},
        {"fr", u"Que veut dire ce mot ?"_s, "spoken.meaning"},
        {"fr", u"Qu'est-ce que ça veut dire ?"_s, "spoken.meaning"},
        {"fr", u"Que veut dire flâner ?"_s, "spoken.meaning", 0, u"flâner"_s},
        {"fr", u"Tourne la page"_s, "reader.page_down"},
        {"nl", u"Volgend hoofdstuk"_s, "reader.next"},
        {"nl", u"Vorige pagina"_s, "reader.page_up"},
        {"nl", u"Markeer dit"_s, "spoken.highlight"},
        {"nl", u"Voorlezen alsjeblieft"_s, "reader.read_aloud"},
        {"nl", u"Ga naar hoofdstuk vijf"_s, "spoken.chapter", 5},
        {"nl", u"Hoofdstuk eenentwintig"_s, "spoken.chapter", 21},
        {"nl", u"Wat betekent dit woord?"_s, "spoken.meaning"},
        {"nl", u"Wat betekent gezellig?"_s, "spoken.meaning", 0, u"gezellig"_s},
        {"nl", u"Stop met voorlezen"_s, "reader.read_aloud_stop"},
        {"es", u"Siguiente capítulo"_s, "reader.next"},
        {"es", u"Página anterior"_s, "reader.page_up"},
        {"es", u"Resalta esto"_s, "spoken.highlight"},
        {"es", u"Lee en voz alta, por favor"_s, "reader.read_aloud"},
        {"es", u"Ve al capítulo cinco"_s, "spoken.chapter", 5},
        {"es", u"Capítulo treinta y uno"_s, "spoken.chapter", 31},
        {"es", u"¿Qué significa esta palabra?"_s, "spoken.meaning"},
        {"es", u"¿Qué significa sobremesa?"_s, "spoken.meaning", 0, u"sobremesa"_s},
        {"es", u"Que significa esto"_s, "spoken.meaning"},
        {"es", u"Pasa la página"_s, "reader.page_down"},
    };
    return all;
}

// A command for the model: it writes down that it was run, and answers.
[[nodiscard]] QString answeringCommand(const QString &folder, const QByteArray &answer) {
    return Fixtures::script(folder + "/model", "touch \"$(dirname \"$0\")/asked\"\ncat > /dev/null\necho '" + answer + "'\n");
}

void allowQuick(Settings &settings, bool hosted) {
    QJsonObject chosen = settings.capabilities();
    chosen.insert("answers", QJsonObject{{"provider", "command"}, {"hosted", hosted}});
    chosen.insert("quick", QJsonObject{{"provider", "command"}, {"hosted", hosted}});
    settings.setCapabilities(chosen);
}

} // namespace

class VoiceCommandsTests : public QObject {
    Q_OBJECT

private slots:
    // Each command from a transcript in every language: in its own, and
    // whatever language the window is in.
    void eachCommandInEveryLanguage() {
        for (const Said &one : transcripts()) {
            const QString where = QString::fromLatin1(one.language) + u": "_s + one.said;
            const auto heard = VoiceGrammar::understandIn(one.said, QString::fromLatin1(one.language));
            QVERIFY2(heard, qPrintable(where));
            QVERIFY2(heard->command == QLatin1StringView(one.command), qPrintable(where + u" → "_s + heard->command));
            QVERIFY2(heard->number == one.number, qPrintable(where + u" → "_s + QString::number(heard->number)));
            QVERIFY2(heard->words == one.words, qPrintable(where + u" → "_s + heard->words));
            for (const QString &window : {u"en"_s, u"de"_s, u"fr"_s, u"nl"_s, u"es-MX"_s, u"ar"_s}) {
                const auto anywhere = VoiceGrammar::understand(one.said, window);
                QVERIFY2(anywhere && anywhere->command == QLatin1StringView(one.command),
                         qPrintable(where + u" in a window in "_s + window));
            }
        }
        // Every language has each of the six.
        for (const QString &language : VoiceGrammar::languages())
            for (const char *command :
                 {"reader.next", "reader.page_up", "spoken.highlight", "reader.read_aloud", "spoken.chapter", "spoken.meaning"})
                QVERIFY2(std::ranges::any_of(transcripts(),
                                             [&](const Said &one) {
                                                 return language == QLatin1StringView(one.language)
                                                        && QLatin1StringView(one.command) == command;
                                             }),
                         qPrintable(language + u" "_s + QLatin1StringView(command)));
    }

    // What isn't a phrase is nothing, never a guess: the model's to try.
    void otherWordsAreNothing() {
        for (const QString &said : {u"I wonder what the weather is like"_s, u"next chapter of my life is going to be great"_s, u""_s,
                                    u"…"_s, u"go to chapter banana"_s, u"highlights of the season"_s})
            QVERIFY2(!VoiceGrammar::understand(said, u"en"_s), qPrintable(said));
        // Highlighting and showing the highlights are told apart.
        QCOMPARE(VoiceGrammar::understand(u"Show my highlights"_s, u"en"_s)->command, u"reader.annotations"_s);
    }

    // The grammar's commands are the table's, and the three only said; the
    // key that listens is never one of them.
    void theGrammarIsOverTheCommandTable() {
        const QStringList spoken{QLatin1StringView(VoiceGrammar::chapter), QLatin1StringView(VoiceGrammar::highlight),
                                 QLatin1StringView(VoiceGrammar::meaning)};
        for (const QString &command : VoiceGrammar::commands()) {
            const bool inTable = std::ranges::any_of(CommandTable::builtIns, [&](const CommandTable::BuiltIn &one) {
                return QLatin1StringView(one.id.data(), static_cast<qsizetype>(one.id.size())) == command;
            });
            QVERIFY2(inTable || spoken.contains(command), qPrintable(command));
            QVERIFY(command != u"reader.voice"_s);
        }
        QVERIFY(std::ranges::any_of(CommandTable::builtIns, [](const CommandTable::BuiltIn &one) { return one.id == "reader.voice"; }));
        // The model is told of every one of them, each with what it does.
        const QString told = VoiceCommands::instructions();
        for (const QString &command : VoiceGrammar::commands())
            QVERIFY2(told.contains(command + u": "_s) && !told.contains(command + u": \n"_s), qPrintable(command));
    }

    void numbersInFiguresAndWords() {
        QCOMPARE(VoiceGrammar::number(u"12"_s, u"en"_s), 12);
        QCOMPARE(VoiceGrammar::number(u"seven"_s, u"en"_s), 7);
        QCOMPARE(VoiceGrammar::number(u"forty-two"_s, u"en"_s), 42);
        QCOMPARE(VoiceGrammar::number(u"zwölf"_s, u"de"_s), 12);
        QCOMPARE(VoiceGrammar::number(u"dreiundvierzig"_s, u"de"_s), 43);
        QCOMPARE(VoiceGrammar::number(u"dix-sept"_s, u"fr"_s), 17);
        QCOMPARE(VoiceGrammar::number(u"één"_s, u"nl"_s), 1);
        QCOMPARE(VoiceGrammar::number(u"drieëntwintig"_s, u"nl"_s), 23);
        QCOMPARE(VoiceGrammar::number(u"dieciséis"_s, u"es-MX"_s), 16);
        QCOMPARE(VoiceGrammar::number(u"cuarenta y dos"_s, u"es"_s), 42);
        QCOMPARE(VoiceGrammar::number(u"0"_s, u"en"_s), std::nullopt);
        QCOMPARE(VoiceGrammar::number(u"banana"_s, u"en"_s), std::nullopt);
    }

    // Chapter 5 is the one its title numbers, front matter or not; else the fifth at the top.
    void aChapterByItsTitleOrItsPlace() {
        const std::vector<VoiceGrammar::Entry> numbered{
            {u"Cover"_s, 0},   {u"Contents"_s, 0}, {u"Chapter 1"_s, 0}, {u"1.1 A start"_s, 1}, {u"Chapter 2: The Ferry"_s, 0},
            {u"Part II"_s, 0}, {u"Chapter V"_s, 0}};
        QCOMPARE(VoiceGrammar::chapterIn(numbered, 1), 2);
        QCOMPARE(VoiceGrammar::chapterIn(numbered, 2), 4);
        QCOMPARE(VoiceGrammar::chapterIn(numbered, 5), 6);
        const std::vector<VoiceGrammar::Entry> named{{u"The Harbour"_s, 1}, {u"The Ferry"_s, 1}, {u"On deck"_s, 2}, {u"The Island"_s, 1}};
        QCOMPARE(VoiceGrammar::chapterIn(named, 3), 3);
        QCOMPARE(VoiceGrammar::chapterIn(named, 4), -1);
        QCOMPARE(VoiceGrammar::chapterIn({}, 1), -1);

        const VoiceCommands commands({});
        const QVariantList toc{QVariantMap{{"title", "One"}, {"level", 0}}, QVariantMap{{"title", "Two"}, {"level", 0}}};
        QCOMPARE(commands.chapter(toc, 2), 1);
    }

    // The grammar's at once; the model is not asked.
    void theGrammarFirst() {
        AiRequest sent;
        int made = 0;
        VoiceCommands commands([&](QString *) -> std::unique_ptr<AiProvider> {
            ++made;
            return std::make_unique<CommandAi>(u"reader.end"_s, &sent);
        });
        QSignalSpy understood(&commands, &VoiceCommands::understood);
        commands.understand(u"Go to chapter 5."_s, u"en"_s);
        QCOMPARE(understood.size(), 1);
        QCOMPARE(understood.first().at(0).toString(), u"spoken.chapter"_s);
        QCOMPARE(understood.first().at(1).toInt(), 5);
        QCOMPARE(understood.first().at(3).toString(), u"Go to chapter 5."_s);
        QCOMPARE(made, 0);
    }

    // What the grammar doesn't know is the model's, as quick work, sent only the words said.
    void theModelForWhatTheGrammarDoesntKnow() {
        AiRequest sent;
        VoiceCommands commands(
            [&](QString *) -> std::unique_ptr<AiProvider> { return std::make_unique<CommandAi>(u"`reader.end`"_s, &sent); });
        QSignalSpy understood(&commands, &VoiceCommands::understood);
        QSignalSpy lost(&commands, &VoiceCommands::notUnderstood);
        commands.understand(u"Take me to where this chapter finishes"_s, u"en"_s);
        QVERIFY(commands.asking());
        QTRY_COMPARE(understood.size(), 1);
        QCOMPARE(understood.first().at(0).toString(), u"reader.end"_s);
        QCOMPARE(understood.first().at(3).toString(), u"Take me to where this chapter finishes"_s);
        QVERIFY(!commands.asking());
        QCOMPARE(sent.tier, AiRequest::Quick);
        QCOMPARE(sent.messages.size(), 1);
        QCOMPARE(sent.messages.first().text, u"Take me to where this chapter finishes"_s);
        QVERIFY(sent.system.contains(u"reader.next: Go to the next chapter"_s));
        QCOMPARE(lost.size(), 0);

        // A chapter and a word come with their command.
        commands.setAnswerer(
            [&](QString *) -> std::unique_ptr<AiProvider> { return std::make_unique<CommandAi>(u"spoken.chapter 12"_s, &sent); });
        commands.understand(u"skip ahead to the twelfth one"_s, u"en"_s);
        QTRY_COMPARE(understood.size(), 2);
        QCOMPARE(understood.last().at(1).toInt(), 12);

        // An answer that is no command is not understood, and said so.
        for (const QString &answer : {u"none"_s, u"I think you would like to read on."_s, u"reader.voice"_s, u"spoken.chapter none"_s}) {
            commands.setAnswerer(
                [&, answer](QString *) -> std::unique_ptr<AiProvider> { return std::make_unique<CommandAi>(answer, &sent); });
            commands.understand(u"lovely weather today"_s, u"en"_s);
            QTRY_COMPARE(lost.size(), 1);
            QCOMPARE(lost.takeFirst().at(0).toString(), u"lovely weather today"_s);
        }
        QCOMPARE(understood.size(), 2);
    }

    void theModelsAnswerRead() {
        QCOMPARE(VoiceCommands::parse(u"reader.next"_s)->command, u"reader.next"_s);
        QCOMPARE(VoiceCommands::parse(u"**reader.next**.\n"_s)->command, u"reader.next"_s);
        QCOMPARE(VoiceCommands::parse(u"spoken.chapter 7"_s)->number, 7);
        QCOMPARE(VoiceCommands::parse(u"spoken.meaning flâneur"_s)->words, u"flâneur"_s);
        QVERIFY(!VoiceCommands::parse(u"none"_s));
        QVERIFY(!VoiceCommands::parse(u"global.quit"_s));
        QVERIFY(!VoiceCommands::parse(u"spoken.chapter"_s));
    }

    // No model to ask: not understood at once.
    void noModelNotUnderstood() {
        VoiceCommands commands([](QString *why) -> std::unique_ptr<AiProvider> {
            *why = u"no model"_s;
            return nullptr;
        });
        QSignalSpy lost(&commands, &VoiceCommands::notUnderstood);
        commands.understand(u"lovely weather today"_s, u"en"_s);
        QCOMPARE(lost.size(), 1);
        QVERIFY(!commands.asking());
    }

    // Through the registry: a model elsewhere only where hosted is allowed
    // for quick work, and none with the AI Off; the grammar works all the same.
    void theRegistrysModelHostedOnlyWhereAllowed() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        settings.setAiCommand(answeringCommand(dir.path(), "reader.end"));
        settings.setAiProvider("command");
        allowQuick(settings, false);
        VoiceCommands commands(AiRegistry::answerer(settings));
        QSignalSpy understood(&commands, &VoiceCommands::understood);
        QSignalSpy lost(&commands, &VoiceCommands::notUnderstood);
        const QString asked = dir.filePath("asked");

        commands.understand(u"Take me to where this chapter finishes"_s, u"en"_s);
        QTRY_COMPARE(lost.size(), 1);
        QVERIFY(!QFile::exists(asked));

        allowQuick(settings, true);
        commands.understand(u"Take me to where this chapter finishes"_s, u"en"_s);
        QTRY_COMPARE(understood.size(), 1);
        QCOMPARE(understood.first().at(0).toString(), u"reader.end"_s);
        QVERIFY(QFile::exists(asked));
        QFile::remove(asked);

        settings.setAiProvider("off");
        commands.understand(u"Take me to where this chapter finishes"_s, u"en"_s);
        QTRY_COMPARE(lost.size(), 2);
        QVERIFY(!QFile::exists(asked));
        commands.understand(u"Next chapter"_s, u"en"_s);
        QCOMPARE(understood.size(), 2);
    }
};

QTEST_MAIN(VoiceCommandsTests)
#include "voicecommands_tests.moc"
