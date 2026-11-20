// A place in a book (#88): one type, BookPlace, for the reading place, the
// graph's moments and the annotations, ordered as the book is read and
// written the one way; a Passage is two of them and the words between. The
// rows a place is kept in are what they were before it was one type.

#include <QDir>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include "bookplace.h"
#include "changelog.h"
#include "fixtures.h"
#include "graph.h"
#include "library.h"

class BookPlaceTests : public QObject {
    Q_OBJECT

private slots:
    void placesAreInTheOrderTheBookIsRead() {
        const BookPlace start{}, early{.section = 0, .fraction = 0.5}, next{.section = 1, .fraction = 0};
        QVERIFY(start < early);
        QVERIFY(early < next);
        QVERIFY((next == BookPlace{.section = 1, .fraction = 0}));
        QVERIFY(early != next);
        // A later section is later, however far into the earlier one.
        QVERIFY((BookPlace{.section = 0, .fraction = 1.0} < BookPlace{.section = 1, .fraction = 0}));
    }

    void aPlaceIsWrittenOneWay() {
        const BookPlace place{.section = 2, .fraction = 0.375};
        QCOMPARE(place.toJson(), (QJsonObject{{"section", 2}, {"fraction", 0.375}}));
        QCOMPARE(BookPlace::fromJson(place.toJson()), place);
        // Beside what else is said of it, as the reading place is.
        QCOMPARE(place.toJson({{"percent", 40}}), (QJsonObject{{"section", 2}, {"fraction", 0.375}, {"percent", 40}}));
        // What an object leaves out is the start, or what the caller says.
        QCOMPARE(BookPlace::fromJson({}), BookPlace{});
        QCOMPARE(BookPlace::fromJson({{"section", 3}}, {.section = 0, .fraction = 1.0}), (BookPlace{.section = 3, .fraction = 1.0}));
        QCOMPARE(BookPlace::fromJson({{"fraction", 0.5}}, {.section = 4, .fraction = 0}), (BookPlace{.section = 4, .fraction = 0.5}));
    }

    void aGraphPlaceIsAPlaceInOneBook() {
        const GraphPlace here{7, 1, 0.5}, sameElsewhere{8, 1, 0.5}, later{7, 2, 0};
        QCOMPARE(here.inBook(), (BookPlace{.section = 1, .fraction = 0.5}));
        QVERIFY(here.inBook() == sameElsewhere.inBook());
        QVERIFY(!(here == sameElsewhere));
        QVERIFY(here == (GraphPlace{7, BookPlace{.section = 1, .fraction = 0.5}}));
        QVERIFY(here.notAfter(later));
        QVERIFY(here.notAfter(here));
        QVERIFY(!later.notAfter(here));
    }

    void aPassageIsTwoPlacesAndItsWords() {
        const Passage passage{.from = {.section = 1, .fraction = 0.25},
                              .to = {.section = 1, .fraction = 0.3},
                              .quote = "the storm broke",
                              .before = "Then ",
                              .after = " over the hills"};
        QVERIFY(passage.contains({.section = 1, .fraction = 0.25}));
        QVERIFY(passage.contains({.section = 1, .fraction = 0.3}));
        QVERIFY(!passage.contains({.section = 1, .fraction = 0.31}));
        QVERIFY(!passage.contains({.section = 0, .fraction = 0.27}));
        QCOMPARE(Passage::fromJson(passage.toJson()), passage);
        QCOMPARE(passage.toJson().value("from").toObject(), passage.from.toJson());
        // Words it has none of are left out; a passage of one place ends where it starts.
        const Passage point{.from = {.section = 2, .fraction = 0.5}, .to = {.section = 2, .fraction = 0.5}};
        QCOMPARE(point.toJson().keys(), (QStringList{"from", "to"}));
        QCOMPARE(Passage::fromJson({{"from", point.from.toJson()}}), point);
    }

    // The reading place, a bookmark and the AI's notes keep their place in
    // the same columns as before: no data changes meaning.
    void placesAreKeptInTheSameRows() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("a.epub")));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const qint64 id = library.add(dir.filePath("a.epub")).id;
        QVERIFY(id > 0);
        library.setProgress(id, {.place = {.section = 2, .fraction = 0.375}, .percent = 40, .updated = 1794700000});
        QVERIFY(
            library.addAnnotation(
                id, {.id = 0, .kind = "bookmark", .place = {.section = 1, .fraction = 0.25}, .text = "The storm", .note = {}, .created = 0})
            > 0);
        library.setNotes(id, "recap", {.place = {.section = 3, .fraction = 0.5}, .text = "So far.", .updated = 0});

        const auto row = [&](const QString &sql) {
            QSqlQuery query = ChangeLog::query(library, sql, {id});
            return query.next() ? std::pair{query.value(0).toInt(), query.value(1).toDouble()} : std::pair{-1, -1.0};
        };
        QCOMPARE(row("SELECT section, fraction FROM progress WHERE book_id = ?"), (std::pair{2, 0.375}));
        QCOMPARE(row("SELECT section, fraction FROM annotations WHERE book_id = ?"), (std::pair{1, 0.25}));
        QCOMPARE(row("SELECT section, fraction FROM notes WHERE book_id = ? AND kind = 'recap'"), (std::pair{3, 0.5}));
        QCOMPARE(library.progress(id).place, (BookPlace{.section = 2, .fraction = 0.375}));
        QCOMPARE(library.annotations(id).first().place, (BookPlace{.section = 1, .fraction = 0.25}));
        QCOMPARE(library.notes(id, "recap").place, (BookPlace{.section = 3, .fraction = 0.5}));
    }
};

QTEST_MAIN(BookPlaceTests)
#include "bookplace_tests.moc"
