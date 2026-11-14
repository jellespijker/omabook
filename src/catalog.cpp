#include "catalog.h"

#include <QRegularExpression>
#include <QSet>

namespace {

// A CSV field: quoted when it holds a comma, a quote or a line break.
QString csv(QString field) {
    static const QRegularExpression special("[\",\\r\\n]");
    if (!field.contains(special))
        return field;
    return "\"" + field.replace("\"", "\"\"") + "\"";
}

// BibTeX reads braces and backslashes as its own.
QString bib(QString value) {
    return value.replace("\\", "\\textbackslash{}").replace("{", "\\{").replace("}", "\\}");
}

QByteArray makeCsv(const QList<Book> &books) {
    QStringList lines{"id,title,authors,series,series_index,publisher,published,language,isbn,tags,formats,percent_read"};
    for (const Book &book : books) {
        const BookMeta &meta = book.meta;
        lines << QStringList{QString::number(book.id), csv(meta.title), csv(meta.authorText()), csv(meta.series),
                             meta.series.isEmpty() ? QString() : QString::number(meta.seriesIndex), csv(meta.publisher),
                             csv(meta.published), csv(meta.language), csv(meta.isbn), csv(meta.tags.join(", ")),
                             csv(book.formats.join(" ")), QString::number(qRound(book.percent))}
                     .join(',');
    }
    return (lines.join("\r\n") + "\r\n").toUtf8();
}

QByteArray makeBibtex(const QList<Book> &books) {
    QString out;
    QSet<QString> keys;
    for (const Book &book : books) {
        const BookMeta &meta = book.meta;
        // "herbert1965dune", made unique with the book's number when it isn't.
        static const QRegularExpression notWord("[^a-z0-9]");
        const QString year = meta.published.left(4);
        QString key = (meta.authors.value(0).section(' ', -1) + year + meta.title.section(' ', 0, 0)).toLower().remove(notWord);
        if (key.isEmpty() || keys.contains(key))
            key += QString("-%1").arg(book.id);
        keys << key;
        QStringList fields{"  title = {" + bib(meta.title) + "}"};
        if (!meta.authors.isEmpty()) fields << "  author = {" + bib(meta.authors.join(" and ")) + "}";
        if (!meta.publisher.isEmpty()) fields << "  publisher = {" + bib(meta.publisher) + "}";
        if (!year.isEmpty()) fields << "  year = {" + year + "}";
        if (!meta.isbn.isEmpty()) fields << "  isbn = {" + bib(meta.isbn) + "}";
        if (!meta.series.isEmpty()) fields << "  series = {" + bib(meta.series) + "}" << QString("  number = {%1}").arg(meta.seriesIndex);
        if (!meta.language.isEmpty()) fields << "  language = {" + bib(meta.language) + "}";
        out += "@book{" + key + ",\n" + fields.join(",\n") + "\n}\n\n";
    }
    return out.toUtf8();
}

} // namespace

QStringList Catalog::formats() { return {"csv", "bibtex"}; }

QString Catalog::describe(const QString &format) {
    return format == "csv" ? "The library as a table, for a spreadsheet." : "The library as references, for LaTeX or a reference manager.";
}

QByteArray Catalog::make(const QList<Book> &books, const QString &format) {
    if (format == "csv") return makeCsv(books);
    if (format == "bibtex") return makeBibtex(books);
    return {};
}
