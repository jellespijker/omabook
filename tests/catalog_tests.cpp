#include <QtTest>

#include "catalog.h"

using namespace Qt::StringLiterals;

// The catalogs the library is written out as.
class CatalogTests : public QObject {
    Q_OBJECT

private slots:
    // What LaTeX reads as its own is written out, each once, and a book in
    // another script has a key of its own (#288).
    void bibtexIsWhatLatexTakes() {
        Book marks, other;
        marks.id = 8;
        marks.meta.title = uR"(C# & the F_Sharp Way: 100% for $5 {sic} ~^ in C:\temp)"_s;
        marks.meta.authors = {u"Ann Lee"_s};
        marks.meta.publisher = u"Smith & Sons"_s;
        other.id = 9;
        other.meta.title = u"雪国"_s;
        other.meta.authors = {u"川端 康成"_s};

        const QString written = QString::fromUtf8(Catalog::make({marks, other}, u"bibtex"_s));
        const QString title = uR"(title = {C\# \& the F\_Sharp Way: 100\% for \$5 \{sic\} )"
                              uR"(\textasciitilde{}\textasciicircum{} in C:\textbackslash{}temp})"_s;
        QVERIFY2(written.contains(title), qPrintable(written));
        QVERIFY2(written.contains(uR"(publisher = {Smith \& Sons})"_s), qPrintable(written));
        QVERIFY2(written.contains(u"@book{leec,"_s), qPrintable(written));
        QVERIFY2(written.contains(u"@book{book9,"_s), qPrintable(written));
        QVERIFY2(written.contains(u"title = {雪国}"_s), qPrintable(written));
    }
};

QTEST_GUILESS_MAIN(CatalogTests)
#include "catalog_tests.moc"
