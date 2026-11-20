// Hyphenation of justified text (src/hyphenation.h): words broken as
// libhyphen breaks them, justified paragraphs given soft hyphens and laid
// out with hyphens at their lines' ends in English, German and Dutch, the
// text read without them at the positions it has without them, the time a
// long chapter takes, and the switch in the Aa panel.
//
// The languages' tests need Arch's hyphen-en, hyphen-de and hyphen-nl, in
// /usr/share/hyphen or a folder in OMABOOK_HYPHENATION_PATH; without them
// they are skipped, saying so. OMABOOK_SHOTS=<folder> saves a picture of
// the reader with each language's text at a few sizes.

#include <QAbstractTextDocumentLayout>
#include <QClipboard>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QPainter>
#include <QQuickStyle>
#include <QQuickTextDocument>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QtTest>

#include "desk.h"
#include "hyphenation.h"
#include "pagelayout.h"
#include "readingformat.h"
#include "textmarks.h"

using Hyphenation::Dictionary;
using Hyphenation::softHyphen;

namespace {

[[nodiscard]] Dictionary dictionary(const QByteArray &file) {
    auto parsed = Dictionary::parse(file);
    Q_ASSERT(parsed);
    return std::move(*parsed);
}

// A word with "-" where it may be broken.
[[nodiscard]] QString shown(const Dictionary &dictionary, const QString &word) {
    return dictionary.hyphenated(word).replace(softHyphen, u'-');
}

// The three languages: a paragraph of each, and a word of it as libhyphen
// (and so LibreOffice) breaks it.
struct Language {
    const char *code, *word, *broken;
    QString paragraph;
};
[[nodiscard]] QList<Language> languages() {
    return {{"en", "hyphenation", "hy-phen-ation",
             "Typographical justification distributes the extraordinary responsibility of hyphenation across "
             "paragraphs, and comprehensive internationalization requires an understanding of characteristically "
             "unpredictable vocabulary in every language a book is written in."},
            {"de", "Silbentrennung", "Sil-ben-tren-nung",
             "Die Silbentrennung ist für den Blocksatz unverzichtbar, denn ohne Trennung entstehen bei Wörtern wie "
             "Donaudampfschifffahrtsgesellschaft, Bundesverfassungsgericht und Geschwindigkeitsbegrenzung unschöne "
             "Lücken zwischen den Wörtern einer Zeile."},
            {"nl", "woordafbreking", "woord-af-bre-king",
             "De woordafbreking is onmisbaar voor uitgevulde tekst, want zonder afbreekpatronen ontstaan bij "
             "wetenschappelijke verhandelingen en ongelooflijk lange samenstellingen zoals "
             "arbeidsongeschiktheidsverzekering lelijke gaten tussen de woorden van een regel."}};
}

// A chapter laid out as the reader lays it out, `width` pixels wide.
struct Laid {
    QTextDocument document;
    Laid(const QString &html, double width) {
        document.setDefaultFont(PageLayout::bodyFont({}, 18));
        document.setHtml(html);
        document.setTextWidth(width);
        document.documentLayout()->documentSize();
    }
    // How many lines end at a soft hyphen: are broken in a word.
    [[nodiscard]] int brokenLines() const {
        int broken = 0;
        for (QTextBlock block = document.begin(); block.isValid(); block = block.next())
            for (int i = 0; i + 1 < block.layout()->lineCount(); ++i) {
                const QTextLine line = block.layout()->lineAt(i);
                broken += block.text().at(line.textStart() + line.textLength() - 1) == softHyphen;
            }
        return broken;
    }
};

[[nodiscard]] QString justified(const QString &paragraphs, const Dictionary *dictionary, const QString &hyphenation = "on") {
    return ReadingFormat::setChapter(paragraphs, {{"align", "justify"}, {"hyphenation", hyphenation}}, QColor("#123456"), dictionary);
}

} // namespace

class HyphenationTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // Liang's patterns as libhyphen reads them: the fewest letters around
    // a break, a character set of its own, and its automaton, which takes
    // at each letter only the pattern its state stands for.
    void wordsAreBrokenAsLibhyphenBreaksThem() {
        const Dictionary liang = dictionary("UTF-8\nLEFTHYPHENMIN 2\nRIGHTHYPHENMIN 3\n% Liang's own example\n"
                                            "hy3ph\nhe2n\nhena4\nhen5at\n1na\nn2at\n1tio\n2io\no2n\n");
        QCOMPARE(shown(liang, "hyphenation"), QString("hy-phen-ation"));
        QCOMPARE(shown(liang, "Hyphenation"), QString("Hy-phen-ation"));
        QCOMPARE(liang.points(u"hyphenation"), (QList<qsizetype>{2, 6}));
        // "a1b" breaks "yyabyy"; in "xxabyy" the automaton is in the state
        // of "xab" (no break) at the b, and libhyphen doesn't look further.
        const Dictionary automaton = dictionary("UTF-8\na1b\nxab\n");
        QCOMPARE(shown(automaton, "yyabyy"), QString("yya-byy"));
        QCOMPARE(shown(automaton, "xxabyy"), QString("xxabyy"));
        // Two levels: where a compound's words meet, then each word.
        const Dictionary compound = dictionary("UTF-8\nb1c\nNEXTLEVEL\na1a\nd1d\n");
        QCOMPARE(shown(compound, "aaaabcddd"), QString("aa-a-ab-cd-dd"));
        // A dictionary in Latin-1, as the German and Dutch ones are.
        const Dictionary latin = dictionary(QByteArray("ISO8859-1\n")
                                            + "\xfc"
                                              "1b\n");
        QCOMPARE(shown(latin, QString::fromUtf8("Tüber")), QString::fromUtf8("Tü-ber"));
        QVERIFY(!Dictionary::parse("UTF-8\n% nothing\n"));
        QVERIFY(!Dictionary::parse("NO-SUCH-SET\na1b\n"));
    }

    // Only the words of the text: not in tags, entities, capitals, numbers,
    // addresses, nor a word the book broke itself; taking the soft hyphens
    // out gives back what went in.
    void onlyTheTextsWordsAreBroken() {
        const Dictionary liang = dictionary("UTF-8\nhy3ph\nhe2n\nhena4\nhen5at\n1na\nn2at\n1tio\n2io\no2n\n");
        const QString html = QString::fromUtf8("<i>hyphenation</i> <a href=\"#hyphenation\">x</a> &hyphenation; HYPHENATION hyphenation2 "
                                               "www.hyphenation.org snake_hyphenation hyphe­nation hyphenation.");
        const QString broken = Hyphenation::hyphenateHtml(html, liang);
        QCOMPARE(QString(broken).remove(softHyphen), QString(html).remove(softHyphen));
        QCOMPARE(broken.count(softHyphen), 2 * 2 + 1);
        QVERIFY(broken.startsWith(QString("<i>hy­phen­ation</i> <a href=\"#hyphenation\">")));
        QVERIFY(broken.endsWith(QString(" hyphe­nation hy­phen­ation.")));
    }

    // The format: justified paragraphs only, not with hyphenation off, not
    // without a dictionary; headings and paragraphs the book centred stay as they are.
    void onlyJustifiedParagraphsAreHyphenated() {
        const Dictionary liang = dictionary("UTF-8\nhy3ph\nhe2n\nhena4\nhen5at\n1na\nn2at\n1tio\n2io\no2n\n");
        const QString chapter = "<h2>hyphenation</h2><p>hyphenation</p><p align=\"center\">hyphenation</p>";
        const QString set = justified(chapter, &liang);
        QCOMPARE(set.count(softHyphen), 2);
        QVERIFY(set.contains(QString("<h2>hyphenation</h2><p align=\"justify\">hy­phen­ation</p>")));
        QVERIFY(!justified(chapter, &liang, "off").contains(softHyphen));
        QVERIFY(!justified(chapter, nullptr).contains(softHyphen));
        QVERIFY(!ReadingFormat::setChapter(chapter, {{"align", "start"}}, QColor("#123456"), &liang).contains(softHyphen));
        QVERIFY(ReadingFormat::keys().contains("hyphenation"));
        QVERIFY(ReadingFormat::problem("hyphenation", "off").isEmpty());
        QCOMPARE(ReadingFormat::problem("hyphenation", "sometimes"), QString("hyphenation is on or off; sometimes is not."));
    }

    // The text without soft hyphens, and each position in it, is what it
    // is without hyphenation; and back.
    void positionsAreThoseOfTheTextWithoutSoftHyphens() {
        const QString plain = "Ilse Penhallow walked to the harbour.";
        const QString shown = QString("Ilse Pen­hal­low walked to the har­bour.");
        const Hyphenation::Plain read(shown);
        QCOMPARE(read.text(), plain);
        for (qsizetype at = 0; at < plain.size(); ++at) {
            QCOMPARE(shown.at(read.toShown(at)), plain.at(at));
            QCOMPARE(read.toPlain(read.toShown(at)), at);
        }
        // "Penhallow", plain 5 to 14, is shown from 5 to 16.
        QCOMPARE(read.toShown(5), qsizetype(5));
        QCOMPARE(read.toShownEnd(14), qsizetype(16));
        QCOMPARE(read.toPlain(shown.size()), plain.size());
        QCOMPARE(Hyphenation::Plain(plain).toShown(7), qsizetype(7));
    }

    // The graph's marks and a moment's words are found through soft
    // hyphens, and given where they are shown.
    void namesAndMomentsAreFoundPastSoftHyphens() {
        const QString shown = QString("Ilse Pen­hal­low walked to the har­bour.");
        const TextMarks::Matcher matcher(QList<TextMarks::Thing>{{1, "character", "Ilse Penhallow", {}, {}, {}, {}}});
        const QList<TextMarks::Mark> marks = matcher.matchShown(shown);
        QCOMPARE(marks.size(), 1);
        QCOMPARE(shown.mid(marks.first().start, marks.first().length), QString("Ilse Pen­hal­low"));
        QTextDocument document;
        document.setHtml("<p>" + shown + "</p>");
        const auto found = TextMarks::nearest(document, "the harbour", 0.5);
        QVERIFY(found);
        QCOMPARE(document.toPlainText().mid(found->first, found->second - found->first), QString("the har­bour"));
    }

    void englishGermanAndDutchLinesEndInHyphens_data() {
        QTest::addColumn<QString>("code");
        QTest::addColumn<QString>("word");
        QTest::addColumn<QString>("broken");
        QTest::addColumn<QString>("paragraph");
        for (const Language &language : languages())
            QTest::newRow(language.code) << QString(language.code) << QString::fromUtf8(language.word) << QString::fromUtf8(language.broken)
                                         << language.paragraph;
    }
    // Justified text in each language, laid out narrow: some of its lines
    // end in a word broken by its rules, none with hyphenation off; and
    // what the page holds, without its soft hyphens, is the same either way.
    void englishGermanAndDutchLinesEndInHyphens() {
        QFETCH(QString, code);
        QFETCH(QString, word);
        QFETCH(QString, broken);
        QFETCH(QString, paragraph);
        const Dictionary *rules = Hyphenation::forLanguage(code);
        if (!rules)
            QSKIP(qPrintable("No hyphenation dictionary for " + code + ": install hyphen-" + code + "."));
        QCOMPARE(shown(*rules, word), broken);
        QCOMPARE(Hyphenation::forLanguage(code + "-" + code.toUpper()), rules);
        const QString chapter = "<p>" + paragraph + "</p><p>" + paragraph + "</p>";
        for (const double width : {240.0, 320.0, 420.0}) {
            const Laid on(justified(chapter, rules), width), off(justified(chapter, rules, "off"), width);
            QVERIFY2(on.brokenLines() > 0, qPrintable(QString("at %1 pixels").arg(width)));
            QCOMPARE(off.brokenLines(), 0);
            QCOMPARE(Hyphenation::Plain(on.document.toPlainText()).text(), off.document.toPlainText());
        }
    }

    void aLanguageWithoutADictionaryIsNotHyphenated() {
        QVERIFY(!Hyphenation::forLanguage(""));
        QVERIFY(!Hyphenation::forLanguage("xx"));
        QVERIFY(!Hyphenation::forLanguage("Klingon"));
        if (Hyphenation::forLanguage("en"))
            QCOMPARE(Hyphenation::forLanguage("eng"), Hyphenation::forLanguage("en"));
    }

    // A long chapter (some 300,000 characters) is hyphenated in a fraction
    // of what laying it out takes, and laid out with its soft hyphens in
    // not much more time than without.
    void aLongChapterIsHyphenatedAndLaidOutQuickly() {
        const Dictionary *rules = Hyphenation::forLanguage("de");
        if (!rules)
            QSKIP("No German hyphenation dictionary: install hyphen-de.");
        QString chapter;
        for (int i = 0; chapter.size() < 300000; ++i)
            chapter += "<p>" + languages().at(i % 3).paragraph + " " + languages().at((i + 1) % 3).paragraph + "</p>";
        QElapsedTimer clock;
        clock.start();
        const QString on = justified(chapter, rules);
        const double hyphenating = clock.nsecsElapsed() / 1e6;
        const QString off = justified(chapter, rules, "off");
        const auto layout = [](const QString &html) {
            QElapsedTimer timer;
            timer.start();
            const Laid laid(html, 420);
            return timer.nsecsElapsed() / 1e6;
        };
        // The faster of two, so a busy moment of the machine doesn't count.
        const double plain = std::min(layout(off), layout(off)), hyphenated = std::min(layout(on), layout(on));
        qInfo("%lld characters, %lld soft hyphens: hyphenated in %.1f ms; laid out in %.1f ms, %.1f ms without them",
              qlonglong(chapter.size()), qlonglong(on.count(softHyphen)), hyphenating, hyphenated, plain);
        QVERIFY(on.count(softHyphen) > 10000);
        // Generous, so a busy machine doesn't fail it.
        QVERIFY2(hyphenating < 1000, qPrintable(QString::number(hyphenating)));
        QVERIFY2(hyphenated < 2 * plain + 100, qPrintable(QString("%1 against %2").arg(hyphenated).arg(plain)));
    }

    // In the window: a justified book in each language shows broken words
    // at its lines' ends, at a few sizes; the Aa panel's switch turns it
    // off; what is copied has no soft hyphens.
    void theReaderHyphenatesAndTheSwitchTurnsItOff() {
        if (!Hyphenation::forLanguage("en") || !Hyphenation::forLanguage("de") || !Hyphenation::forLanguage("nl"))
            QSKIP("Needs hyphen-en, hyphen-de and hyphen-nl.");
        Desk desk("en", {}, false);
        QList<qint64> books;
        for (const Language &language : languages()) {
            QString body = QString("<h1>%1</h1>").arg(language.code);
            for (int i = 0; i < 12; ++i)
                body += "<p>" + language.paragraph + "</p>";
            const QString file = desk.dir.filePath(QString("%1.epub").arg(language.code));
            QVERIFY(Fixtures::chapteredEpub(file, QString("Book %1").arg(language.code), {{language.code, body}}));
            const qint64 id = desk.backend->library()->add(file).id;
            QVERIFY(id > 0);
            BookMeta meta = desk.backend->library()->book(id)->meta;
            meta.language = language.code;
            QVERIFY(desk.backend->library()->setMeta(id, meta));
            books << id;
        }
        desk.settings->setOption("align", "justify");
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        const auto document = [&] {
            QObject *text = desk.find("flowText");
            auto *quick = text ? text->property("textDocument").value<QQuickTextDocument *>() : nullptr;
            return quick ? quick->textDocument() : nullptr;
        };
        const auto brokenLines = [&] {
            QTextDocument *shownDocument = document();
            int broken = 0;
            for (QTextBlock block = shownDocument ? shownDocument->begin() : QTextBlock(); block.isValid(); block = block.next())
                for (int i = 0; i + 1 < block.layout()->lineCount(); ++i) {
                    const QTextLine line = block.layout()->lineAt(i);
                    broken += block.text().at(line.textStart() + line.textLength() - 1) == softHyphen;
                }
            return broken;
        };
        for (qsizetype i = 0; i < books.size(); ++i) {
            desk.reader->openBook(books.at(i));
            QTRY_VERIFY(desk.reader->isOpen() && desk.reader->bookId() == books.at(i));
            QCOMPARE(desk.reader->format()->hyphenationLanguage().isEmpty(), false);
            for (const QSize size : {QSize(720, 820), QSize(1000, 760), QSize(1500, 900)}) {
                QVERIFY(desk.resize(size));
                QTRY_VERIFY(document());
                QTRY_VERIFY2(brokenLines() > 0, qPrintable(QString("%1 at %2 wide").arg(languages().at(i).code).arg(size.width())));
                desk.shot(QString("hyphenation-%1-%2").arg(languages().at(i).code).arg(size.width()));
            }
        }
        // Copied, a broken word is whole.
        QVERIFY(desk.resize(QSize(720, 820)));
        QTRY_VERIFY(document());
        QMetaObject::invokeMethod(desk.find("flowText"), "selectAll");
        QMetaObject::invokeMethod(desk.find("flowReader"), "copySelection");
        if (QGuiApplication::clipboard()->text().isEmpty())
            qInfo("No clipboard here to look at.");
        else {
            QVERIFY(!QGuiApplication::clipboard()->text().contains(softHyphen));
            QVERIFY(QGuiApplication::clipboard()->text().contains("woordafbreking"));
        }
        QVERIFY(!desk.find("flowReader")->property("selectedText").toString().contains(softHyphen));
        // The Aa panel says by which language, and turns it off.
        desk.find("readerView")->setProperty("showFormat", true);
        QTRY_VERIFY(desk.shows("formatHyphenation"));
        QObject *toggle = desk.find("formatHyphenation");
        QVERIFY(toggle->property("checked").toBool());
        QCOMPARE(desk.find("formatHyphenationName")->property("text").toString(),
                 QString("Words are broken at the end of a line by the rules of Nederlands"));
        desk.shot("hyphenation-panel");
        QMetaObject::invokeMethod(toggle, "toggle");
        QMetaObject::invokeMethod(toggle, "toggled");
        QTRY_COMPARE(desk.reader->format()->effective().value("hyphenation").toString(), QString("off"));
        QTRY_COMPARE(brokenLines(), 0);
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }
};

QTEST_MAIN(HyphenationTests)
#include "hyphenation_tests.moc"
