#include "opdsfeed.h"

#include <QHash>
#include <QXmlStreamWriter>
#include <algorithm>

#include "formats.h"

using namespace Qt::StringLiterals;

namespace {

constexpr auto atom = "http://www.w3.org/2005/Atom";

[[nodiscard]] QString when(const QDateTime &time) {
    return (time.isValid() ? time : QDateTime::fromSecsSinceEpoch(0)).toUTC().toString(Qt::ISODate);
}

void link(QXmlStreamWriter &xml, const QString &rel, const QString &href, const QString &type, const QString &title = {}) {
    xml.writeEmptyElement(u"link"_s);
    xml.writeAttribute(u"rel"_s, rel);
    xml.writeAttribute(u"href"_s, href);
    xml.writeAttribute(u"type"_s, type);
    if (!title.isEmpty())
        xml.writeAttribute(u"title"_s, title);
}

// The feed's opening: its namespaces, id, title, time and the links every feed has.
void open(QXmlStreamWriter &xml, const QString &base, const QString &id, const QString &title, const QDateTime &updated) {
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeDefaultNamespace(QString::fromLatin1(atom));
    xml.writeNamespace(u"http://purl.org/dc/terms/"_s, u"dc"_s);
    xml.writeNamespace(u"http://opds-spec.org/2010/catalog"_s, u"opds"_s);
    xml.writeStartElement(QString::fromLatin1(atom), u"feed"_s);
    xml.writeTextElement(u"id"_s, id);
    xml.writeTextElement(u"title"_s, title);
    xml.writeTextElement(u"updated"_s, when(updated));
    xml.writeStartElement(u"author"_s);
    xml.writeTextElement(u"name"_s, u"Omabook"_s);
    xml.writeEndElement();
    link(xml, u"start"_s, base, QString::fromLatin1(OpdsFeed::navigationType));
    link(xml, u"search"_s, base + u"/search.xml"_s, u"application/opensearchdescription+xml"_s);
}

void entry(QXmlStreamWriter &xml, const QString &base, const Book &book) {
    xml.writeStartElement(u"entry"_s);
    xml.writeTextElement(u"title"_s, book.meta.title);
    xml.writeTextElement(u"id"_s, u"urn:omabook:book:%1"_s.arg(book.id));
    xml.writeTextElement(u"updated"_s, when(QDateTime::fromSecsSinceEpoch(std::max(book.added, book.detailsAt))));
    for (const QString &author : book.meta.authors) {
        xml.writeStartElement(u"author"_s);
        xml.writeTextElement(u"name"_s, author);
        xml.writeEndElement();
    }
    if (!book.meta.language.isEmpty())
        xml.writeTextElement(u"http://purl.org/dc/terms/"_s, u"language"_s, book.meta.language);
    if (!book.meta.publisher.isEmpty())
        xml.writeTextElement(u"http://purl.org/dc/terms/"_s, u"publisher"_s, book.meta.publisher);
    if (!book.meta.published.isEmpty())
        xml.writeTextElement(u"http://purl.org/dc/terms/"_s, u"issued"_s, book.meta.published);
    for (const QString &tag : book.meta.tags) {
        xml.writeEmptyElement(u"category"_s);
        xml.writeAttribute(u"term"_s, tag);
        xml.writeAttribute(u"label"_s, tag);
    }
    if (!book.meta.description.isEmpty()) {
        // A description that is HTML is said to be, and escaped as the text of the element.
        xml.writeStartElement(u"content"_s);
        xml.writeAttribute(u"type"_s, book.meta.description.contains('<') ? u"html"_s : u"text"_s);
        xml.writeCharacters(book.meta.description);
        xml.writeEndElement();
    }
    const QString at = base + u"/books/%1"_s.arg(book.id);
    if (book.hasCover) {
        link(xml, u"http://opds-spec.org/image"_s, at + u"/cover"_s, u"image/jpeg"_s);
        link(xml, u"http://opds-spec.org/image/thumbnail"_s, at + u"/cover"_s, u"image/jpeg"_s);
    }
    for (const QString &format : book.formats)
        link(xml, u"http://opds-spec.org/acquisition"_s, at + u"/file/"_s + format, OpdsFeed::mimeType(format), Formats::name(format));
    xml.writeEndElement();
}

} // namespace

QByteArray OpdsFeed::navigation(const QString &base, const QString &id, const QString &title, const QList<Section> &sections,
                                const QDateTime &updated) {
    QByteArray out;
    QXmlStreamWriter xml(&out);
    open(xml, base, id, title, updated);
    link(xml, u"self"_s, base, QString::fromLatin1(navigationType));
    for (const Section &section : sections) {
        xml.writeStartElement(u"entry"_s);
        xml.writeTextElement(u"title"_s, section.title);
        xml.writeTextElement(u"id"_s, section.id);
        xml.writeTextElement(u"updated"_s, when(updated));
        xml.writeStartElement(u"content"_s);
        xml.writeAttribute(u"type"_s, u"text"_s);
        xml.writeCharacters(section.summary);
        xml.writeEndElement();
        link(xml, u"subsection"_s, base + section.href, QString::fromLatin1(section.acquisition ? acquisitionType : navigationType));
        xml.writeEndElement();
    }
    xml.writeEndDocument();
    return out;
}

QByteArray OpdsFeed::acquisition(const QString &base, const Page &page) {
    QByteArray out;
    QXmlStreamWriter xml(&out);
    open(xml, base, page.id, page.title, page.updated);
    const QString type = QString::fromLatin1(acquisitionType);
    link(xml, u"self"_s, base + page.self, type);
    link(xml, u"up"_s, base, QString::fromLatin1(navigationType));
    if (!page.previous.isEmpty())
        link(xml, u"previous"_s, base + page.previous, type);
    if (!page.next.isEmpty())
        link(xml, u"next"_s, base + page.next, type);
    for (const Book &book : page.books)
        entry(xml, base, book);
    xml.writeEndDocument();
    return out;
}

QByteArray OpdsFeed::openSearch(const QString &base) {
    QByteArray out;
    QXmlStreamWriter xml(&out);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeDefaultNamespace(u"http://a9.com/-/spec/opensearch/1.1/"_s);
    xml.writeStartElement(u"http://a9.com/-/spec/opensearch/1.1/"_s, u"OpenSearchDescription"_s);
    xml.writeTextElement(u"ShortName"_s, u"Omabook"_s);
    xml.writeTextElement(u"Description"_s, u"Search the library"_s);
    xml.writeEmptyElement(u"Url"_s);
    xml.writeAttribute(u"type"_s, QString::fromLatin1(acquisitionType));
    xml.writeAttribute(u"template"_s, base + u"/search?q={searchTerms}"_s);
    xml.writeEndDocument();
    return out;
}

QString OpdsFeed::mimeType(const QString &format) {
    static const QHash<QString, QString> types{
        {"epub", "application/epub+zip"},
        {"kepub", "application/kepub+zip"},
        {"pdf", "application/pdf"},
        {"mobi", "application/x-mobipocket-ebook"},
        {"azw3", "application/vnd.amazon.ebook"},
        {"azw", "application/vnd.amazon.ebook"},
        {"fb2", "application/x-fictionbook+xml"},
        {"cbz", "application/vnd.comicbook+zip"},
        {"cbr", "application/vnd.comicbook-rar"},
        {"djvu", "image/vnd.djvu"},
        {"txt", "text/plain"},
        {"md", "text/markdown"},
        {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {"rtf", "application/rtf"},
    };
    return types.value(format.toLower(), u"application/octet-stream"_s);
}
