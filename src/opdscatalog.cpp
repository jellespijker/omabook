#include "opdscatalog.h"

#include <QXmlStreamReader>
#include <algorithm>
#include <iterator>

using namespace Qt::StringLiterals;

namespace {

constexpr auto atom = "http://www.w3.org/2005/Atom"_L1;

[[nodiscard]] OpdsCatalog::Link linkAt(const QXmlStreamReader &xml, const QUrl &address) {
    const QXmlStreamAttributes attributes = xml.attributes();
    return {.rel = attributes.value("rel").toString(),
            .href = address.resolved(QUrl(attributes.value("href").toString())),
            .type = attributes.value("type").toString(),
            .title = attributes.value("title").toString()};
}

// One element of an entry, at its start: what it says of the entry.
void readEntryElement(QXmlStreamReader &xml, const QUrl &address, OpdsCatalog::Entry &entry, bool inAuthor) {
    const QStringView name = xml.name();
    if (name == u"name" && inAuthor)
        entry.authors << xml.readElementText().trimmed();
    else if (name == u"title")
        entry.title = xml.readElementText(QXmlStreamReader::IncludeChildElements).simplified();
    else if (name == u"id")
        entry.id = xml.readElementText().trimmed();
    else if ((name == u"summary" || name == u"content") && entry.summary.isEmpty())
        entry.summary = xml.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
    else if (name == u"link")
        entry.links << linkAt(xml, address);
}

// An entry, from its start to its end.
[[nodiscard]] OpdsCatalog::Entry readEntry(QXmlStreamReader &xml, const QUrl &address) {
    OpdsCatalog::Entry entry;
    bool inAuthor = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isEndElement() && xml.name() == u"entry")
            break;
        if (xml.isEndElement() && xml.name() == u"author")
            inAuthor = false;
        if (!xml.isStartElement() || xml.namespaceUri() != atom)
            continue;
        if (xml.name() == u"author")
            inAuthor = true;
        else
            readEntryElement(xml, address, entry, inAuthor);
    }
    return entry;
}

// One element of the feed itself, at its start.
void readFeedElement(QXmlStreamReader &reader, const QUrl &address, OpdsCatalog::Feed &feed) {
    const QStringView name = reader.name();
    if (name == u"entry")
        feed.entries << readEntry(reader, address);
    else if (name == u"link")
        feed.links << linkAt(reader, address);
    else if (name == u"title" && feed.title.isEmpty())
        feed.title = reader.readElementText().simplified();
    else if (name == u"id" && feed.id.isEmpty())
        feed.id = reader.readElementText().trimmed();
    else if (name == u"author")
        reader.skipCurrentElement();
}

} // namespace

QList<OpdsCatalog::Link> OpdsCatalog::Entry::acquisitions() const {
    QList<Link> found;
    std::ranges::copy_if(links, std::back_inserter(found),
                         [](const Link &link) { return link.rel.startsWith(u"http://opds-spec.org/acquisition"); });
    return found;
}

std::optional<OpdsCatalog::Link> OpdsCatalog::Entry::feed() const {
    const auto found = std::ranges::find_if(links, [](const Link &link) { return link.type.contains(u"profile=opds-catalog"); });
    if (found == links.end() || !acquisitions().isEmpty())
        return std::nullopt;
    return *found;
}

std::optional<OpdsCatalog::Link> OpdsCatalog::Feed::link(const QString &rel) const {
    const auto found = std::ranges::find(links, rel, &Link::rel);
    if (found == links.end())
        return std::nullopt;
    return *found;
}

std::expected<OpdsCatalog::Feed, Failure> OpdsCatalog::parse(const QByteArray &xml, const QUrl &address) {
    QXmlStreamReader reader(xml);
    Feed feed;
    bool inFeed = false;
    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement())
            continue;
        if (!inFeed) {
            if (reader.name() != u"feed" || reader.namespaceUri() != atom)
                return std::unexpected(Failure(Failure::Code::NotAnOpdsFeed, {address.toDisplayString()}));
            inFeed = true;
            continue;
        }
        if (reader.namespaceUri() == atom)
            readFeedElement(reader, address, feed);
    }
    if (!inFeed || reader.hasError())
        return std::unexpected(Failure(Failure::Code::NotAnOpdsFeed, {address.toDisplayString()}));
    return feed;
}
