#include "opfdetails.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QXmlStreamReader>
#include <algorithm>
#include <array>

namespace {

using namespace Qt::StringLiterals;

constexpr QStringView dcNamespace = u"http://purl.org/dc/elements/1.1/";
constexpr QStringView opfNamespace = u"http://www.idpf.org/2007/opf";

// What a scheme names outside this library: not Calibre's number for the
// book, nor the plugin that made it; the ISBN has an identifier of its own.
[[nodiscard]] bool travelsOut(const QString &scheme) {
    return std::ranges::none_of(std::array{u"calibre"_s, u"generated"_s, u"isbn"_s},
                                [&scheme](const QString &own) { return scheme.compare(own, Qt::CaseInsensitive) == 0; });
}

// The ISBN an identifier holds, as the reader reads it (epub.cpp).
[[nodiscard]] QString isbnOf(const QString &identifier) {
    static const QRegularExpression urn("^urn:isbn:", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression isbn("^(97[89])?[0-9]{9}[0-9Xx]$");
    QString digits = identifier;
    digits.remove(urn).remove('-').remove(' ');
    return isbn.match(digits).hasMatch() ? digits.toUpper() : QString();
}

// One element directly inside <metadata>, and where its text is.
struct Child {
    qsizetype from = 0; // where the whitespace before it begins
    qsizetype tag = 0;  // where its start tag begins
    qsizetype to = 0;   // just after its end tag
    QString ns, name, id, refines, property, metaName, scheme, event, text;
};

struct Package {
    bool epub3 = false;
    QString uniqueId;
    QString metadataPrefix, manifestPrefix;
    QHash<QString, QString> prefixes; // namespace → prefix, declared on the package or the metadata
    qsizetype metadataOpen = -1;      // the '>' of the metadata's start tag
    qsizetype manifestEnd = -1;       // just after the manifest's last item
    bool metadataEnded = false;
    QList<Child> children;
};

void declared(const QXmlStreamReader &xml, Package &package) {
    for (const QXmlStreamNamespaceDeclaration &declaration : xml.namespaceDeclarations())
        if (!declaration.prefix().isEmpty())
            package.prefixes.insert(declaration.namespaceUri().toString(), declaration.prefix().toString());
}

[[nodiscard]] Child child(QXmlStreamReader &xml, const QString &text) {
    Child found;
    found.tag = text.lastIndexOf(u'<' + xml.qualifiedName().toString(), xml.characterOffset() - 1);
    found.from = found.tag;
    while (found.from > 0 && text.at(found.from - 1).isSpace())
        --found.from;
    found.ns = xml.namespaceUri().toString();
    found.name = xml.name().toString();
    const QXmlStreamAttributes attributes = xml.attributes();
    for (const QXmlStreamAttribute &attribute : attributes) {
        const auto name = attribute.name();
        const QString value = attribute.value().toString();
        if (name == u"id")
            found.id = value;
        else if (name == u"refines")
            found.refines = value.startsWith(u'#') ? value.mid(1) : value;
        else if (name == u"property")
            found.property = value;
        else if (name == u"name")
            found.metaName = value;
        else if (name == u"scheme")
            found.scheme = value;
        else if (name == u"event")
            found.event = value;
    }
    found.text = xml.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
    if (attributes.hasAttribute(u"content"_s))
        found.text = attributes.value(u"content"_s).toString();
    found.to = text.lastIndexOf(u'>', xml.characterOffset() - 1) + 1;
    return found;
}

// An element begun at this depth: the package, its metadata and what is in
// it, its manifest. False for one read whole here, end and all.
[[nodiscard]] bool begun(QXmlStreamReader &xml, const QString &text, int depth, Package &package) {
    if (depth == 1) {
        package.epub3 = xml.attributes().value(u"version"_s).startsWith(u'3');
        package.uniqueId = xml.attributes().value(u"unique-identifier"_s).toString();
        declared(xml, package);
    } else if (depth == 2 && xml.name() == u"metadata") {
        package.metadataPrefix = xml.prefix().toString();
        package.metadataOpen = text.lastIndexOf(u'>', xml.characterOffset() - 1);
        declared(xml, package);
    } else if (depth == 2 && xml.name() == u"manifest") {
        package.manifestPrefix = xml.prefix().toString();
    } else if (depth == 3 && package.metadataOpen >= 0 && !package.metadataEnded) {
        package.children << child(xml, text);
        return false;
    }
    return true;
}

// Where everything this changes is in the package's text.
[[nodiscard]] std::optional<Package> parsed(const QString &text) {
    QXmlStreamReader xml(text);
    Package package;
    int depth = 0;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            depth += begun(xml, text, depth + 1, package) ? 1 : 0;
            continue;
        }
        if (!xml.isEndElement())
            continue;
        if (depth == 2 && xml.name() == u"metadata")
            package.metadataEnded = true;
        if (depth == 2 && xml.name() == u"manifest")
            package.manifestEnd = text.lastIndexOf(u'>', text.lastIndexOf(u'<', xml.characterOffset() - 1) - 1) + 1;
        --depth;
    }
    // A metadata element written empty (<metadata/>) holds no title to replace.
    if (xml.hasError() || package.metadataOpen < 1 || text.at(package.metadataOpen - 1) == u'/')
        return std::nullopt;
    return package;
}

[[nodiscard]] bool identifierOwned(const Child &child, const OpfDetails::Details &details) {
    if (!isbnOf(child.text).isEmpty() || child.scheme.compare(u"isbn"_s, Qt::CaseInsensitive) == 0)
        return true;
    // Said by its scheme attribute (EPUB 2), or in front of its value (EPUB 3's "goodreads:4671").
    QString scheme = child.scheme;
    if (scheme.isEmpty() && child.text.contains(u':'))
        scheme = child.text.section(u':', 0, 0);
    const QStringList known = details.identifiers.keys();
    return !scheme.isEmpty() && travelsOut(scheme)
           && std::ranges::any_of(known, [&scheme](const QString &key) { return key.compare(scheme, Qt::CaseInsensitive) == 0; });
}

// Whether this writes the element anew: what the library says, in place of
// what the file said. A language, which every EPUB must have, stays when the
// library has none; an EPUB 2's dates of other events than publication stay.
[[nodiscard]] bool owned(const Child &child, const Package &package, const OpfDetails::Details &details, bool newCover) {
    if (child.ns == dcNamespace) {
        if (QStringList{u"title"_s, u"creator"_s, u"description"_s, u"publisher"_s, u"subject"_s}.contains(child.name))
            return true;
        if (child.name == u"language")
            return !details.meta.language.isEmpty();
        if (child.name == u"date")
            return package.epub3 || child.event.isEmpty() || child.event.compare(u"publication"_s, Qt::CaseInsensitive) == 0;
        return child.name == u"identifier" && (package.uniqueId.isEmpty() || child.id != package.uniqueId)
               && identifierOwned(child, details);
    }
    if (child.name != u"meta")
        return false;
    if (QStringList{u"calibre:series"_s, u"calibre:series_index"_s, u"calibre:title_sort"_s}.contains(child.metaName))
        return true;
    if (child.metaName == u"cover")
        return newCover;
    return child.property == u"belongs-to-collection" || child.property == u"dcterms:modified";
}

// Those, and what refines them (an EPUB 3's role of a creator, the type of
// a collection), which would refine nothing once they are gone.
[[nodiscard]] QList<qsizetype> ownedChildren(const Package &package, const OpfDetails::Details &details, bool newCover) {
    QSet<qsizetype> taken;
    QSet<QString> ids;
    for (qsizetype at = 0; at < package.children.size(); ++at)
        if (const Child &one = package.children.at(at); owned(one, package, details, newCover)) {
            taken.insert(at);
            if (!one.id.isEmpty())
                ids.insert(one.id);
        }
    for (bool more = true; more;) {
        more = false;
        for (qsizetype at = 0; at < package.children.size(); ++at) {
            const Child &one = package.children.at(at);
            if (taken.contains(at) || !ids.contains(one.refines))
                continue;
            taken.insert(at);
            if (!one.id.isEmpty() && !ids.contains(one.id)) {
                ids.insert(one.id);
                more = true;
            }
        }
    }
    QList<qsizetype> sorted(taken.begin(), taken.end());
    std::ranges::sort(sorted);
    return sorted;
}

[[nodiscard]] QString attribute(const QString &name, const QString &value) { return u' ' + name + u"=\""_s + value.toHtmlEscaped() + u'"'; }

[[nodiscard]] QString element(const QString &name, const QString &attributes, const QString &text) {
    return u'<' + name + attributes + u'>' + text.toHtmlEscaped() + u"</"_s + name + u'>';
}

// How the elements written are named here.
struct Names {
    QString dc;   // "dc:"
    QString opf;  // "opf:", for an EPUB 2's attributes
    QString meta; // "meta", or "opf:meta" where the metadata is so named
};

[[nodiscard]] QStringList creators(const Package &package, const OpfDetails::Details &details, const Names &names) {
    QStringList written;
    for (qsizetype at = 0; at < details.meta.authors.size(); ++at) {
        const QString author = details.meta.authors.at(at);
        if (!package.epub3) {
            written << element(names.dc + u"creator"_s, attribute(names.opf + u"role"_s, u"aut"_s), author);
            continue;
        }
        const QString id = u"omabook-creator-%1"_s.arg(at + 1);
        written << element(names.dc + u"creator"_s, attribute(u"id"_s, id), author)
                << element(names.meta,
                           attribute(u"refines"_s, u'#' + id) + attribute(u"property"_s, u"role"_s)
                               + attribute(u"scheme"_s, u"marc:relators"_s),
                           u"aut"_s);
    }
    return written;
}

[[nodiscard]] QStringList identifiers(const Package &package, const OpfDetails::Details &details, const Names &names) {
    QStringList written;
    const auto unique =
        package.uniqueId.isEmpty() ? package.children.end() : std::ranges::find(package.children, package.uniqueId, &Child::id);
    const QString isbn = isbnOf(details.meta.isbn);
    if (!isbn.isEmpty() && (unique == package.children.end() || isbnOf(unique->text) != isbn))
        written << (package.epub3 ? element(names.dc + u"identifier"_s, {}, u"urn:isbn:"_s + isbn)
                                  : element(names.dc + u"identifier"_s, attribute(names.opf + u"scheme"_s, u"ISBN"_s), isbn));
    for (auto it = details.identifiers.begin(); it != details.identifiers.end(); ++it) {
        const QString value = it.value().toVariant().toString();
        if (value.isEmpty() || !travelsOut(it.key()))
            continue;
        written << (package.epub3 ? element(names.dc + u"identifier"_s, {}, it.key() + u':' + value)
                                  : element(names.dc + u"identifier"_s, attribute(names.opf + u"scheme"_s, it.key().toUpper()), value));
    }
    return written;
}

[[nodiscard]] QStringList series(const Package &package, const OpfDetails::Details &details, const Names &names) {
    if (details.meta.series.isEmpty())
        return {};
    const QString index = QString::number(details.meta.seriesIndex, 'g', 12);
    QStringList written;
    if (package.epub3) {
        written << element(names.meta, attribute(u"property"_s, u"belongs-to-collection"_s) + attribute(u"id"_s, u"omabook-series"_s),
                           details.meta.series)
                << element(names.meta, attribute(u"refines"_s, u"#omabook-series"_s) + attribute(u"property"_s, u"collection-type"_s),
                           u"series"_s);
        if (details.meta.seriesIndex > 0)
            written << element(names.meta, attribute(u"refines"_s, u"#omabook-series"_s) + attribute(u"property"_s, u"group-position"_s),
                               index);
    }
    written << u'<' + names.meta + attribute(u"name"_s, u"calibre:series"_s) + attribute(u"content"_s, details.meta.series) + u"/>"_s;
    if (details.meta.seriesIndex > 0)
        written << u'<' + names.meta + attribute(u"name"_s, u"calibre:series_index"_s) + attribute(u"content"_s, index) + u"/>"_s;
    return written;
}

[[nodiscard]] QStringList generated(const Package &package, const OpfDetails::Details &details, bool newCover, const Names &names) {
    const BookMeta &meta = details.meta;
    QStringList written{element(names.dc + u"title"_s, {}, meta.title)};
    written << creators(package, details, names);
    const auto plain = [&](const QString &name, const QString &value) {
        if (!value.isEmpty())
            written << element(names.dc + name, {}, value);
    };
    plain(u"language"_s, meta.language);
    plain(u"publisher"_s, meta.publisher);
    plain(u"date"_s, meta.published);
    plain(u"description"_s, meta.description);
    for (const QString &tag : meta.tags)
        plain(u"subject"_s, tag);
    written << identifiers(package, details, names) << series(package, details, names);
    if (package.epub3) {
        const QDateTime modified = details.modified.isValid() ? details.modified : QDateTime::currentDateTimeUtc();
        written << element(names.meta, attribute(u"property"_s, u"dcterms:modified"_s),
                           modified.toUTC().toString(u"yyyy-MM-dd'T'HH:mm:ss'Z'"_s));
    }
    if (newCover)
        written << u'<' + names.meta + attribute(u"name"_s, u"cover"_s)
                       + attribute(u"content"_s, QString::fromLatin1(OpfDetails::addedCoverId)) + u"/>"_s;
    return written;
}

// What goes between two elements of the metadata: the line break and
// indentation before its first one, or nothing when they share a line.
[[nodiscard]] QString separator(const QString &text, const Package &package) {
    if (package.children.isEmpty())
        return {};
    const Child &first = package.children.first();
    const QString space = text.mid(first.from, first.tag - first.from);
    qsizetype line = space.lastIndexOf(u'\n');
    if (line > 0 && space.at(line - 1) == u'\r')
        --line;
    return line < 0 ? space : space.mid(line);
}

constexpr QByteArrayView utf8Bom = "\xEF\xBB\xBF";

// The package's text, without the byte order mark it may begin with; none
// for one in another encoding than UTF-8, whose bytes this would change.
[[nodiscard]] std::optional<QString> decoded(const QByteArray &opf) {
    static const QRegularExpression encoding(R"(^\s*<\?xml[^>]*encoding\s*=\s*["']([^"']+)["'])");
    const QByteArray bytes = opf.startsWith(utf8Bom) ? opf.mid(utf8Bom.size()) : opf;
    const QRegularExpressionMatch said = encoding.match(QString::fromLatin1(bytes.left(200)));
    if (opf.startsWith("\xFE\xFF") || opf.startsWith("\xFF\xFE")
        || (said.hasMatch() && said.captured(1).compare(u"utf-8"_s, Qt::CaseInsensitive) != 0))
        return std::nullopt;
    return QString::fromUtf8(bytes);
}

[[nodiscard]] bool wellFormed(const QString &text) {
    QXmlStreamReader xml(text);
    while (!xml.atEnd())
        xml.readNext();
    return !xml.hasError();
}

struct Edit {
    qsizetype from = 0, to = 0;
    QString with;
};

} // namespace

std::optional<OpfDetails::CoverItem> OpfDetails::coverItem(const QByteArray &opf) {
    QXmlStreamReader xml(opf);
    QString coverId;
    QHash<QString, CoverItem> items;
    std::optional<CoverItem> byProperty;
    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement)
            continue;
        const QXmlStreamAttributes attributes = xml.attributes();
        if (xml.name() == u"meta" && attributes.value(u"name"_s) == u"cover") {
            coverId = attributes.value(u"content"_s).toString();
        } else if (xml.name() == u"item") {
            const CoverItem item{.href = attributes.value(u"href"_s).toString(),
                                 .mediaType = attributes.value(u"media-type"_s).toString().toLower()};
            items.insert(attributes.value(u"id"_s).toString(), item);
            if (!byProperty && attributes.value(u"properties"_s).split(u' ').contains(u"cover-image"))
                byProperty = item;
        }
    }
    if (items.contains(coverId))
        return items.value(coverId);
    return byProperty;
}

std::expected<QByteArray, Failure> OpfDetails::rewritten(const QByteArray &opf, const Details &details, const QString &newCover) {
    const std::optional<QString> text = decoded(opf);
    const std::optional<Package> package = text ? parsed(*text) : std::nullopt;
    if (!package)
        return std::unexpected(Failure(Failure::Code::EpubPackageUnwritable));
    // The prefixes the package already uses, and those it needs declared.
    QString declarations;
    const auto prefix = [&](QStringView ns, const QString &wanted) {
        if (package->prefixes.contains(ns.toString()))
            return package->prefixes.value(ns.toString()) + u':';
        declarations += attribute(u"xmlns:"_s + wanted, ns.toString());
        return wanted + u':';
    };
    const Names names{.dc = prefix(dcNamespace, u"dc"_s),
                      .opf = package->epub3 ? QString() : prefix(opfNamespace, u"opf"_s),
                      .meta = package->metadataPrefix.isEmpty() ? u"meta"_s : package->metadataPrefix + u":meta"_s};
    const QString between = separator(*text, *package);

    QList<Edit> edits;
    for (const qsizetype at : ownedChildren(*package, details, !newCover.isEmpty()))
        edits << Edit{.from = package->children.at(at).from, .to = package->children.at(at).to, .with = {}};
    const QStringList written = generated(*package, details, !newCover.isEmpty(), names);
    edits << Edit{
        .from = package->metadataOpen, .to = package->metadataOpen + 1, .with = declarations + u'>' + between + written.join(between)};
    if (!newCover.isEmpty() && package->manifestEnd > 0) {
        const QString item = package->manifestPrefix.isEmpty() ? u"item"_s : package->manifestPrefix + u":item"_s;
        edits << Edit{.from = package->manifestEnd,
                      .to = package->manifestEnd,
                      .with = between + u'<' + item + attribute(u"id"_s, QString::fromLatin1(addedCoverId)) + attribute(u"href"_s, newCover)
                              + attribute(u"media-type"_s, u"image/jpeg"_s)
                              + (package->epub3 ? attribute(u"properties"_s, u"cover-image"_s) : QString()) + u"/>"_s};
    }
    // From the end, so each edit's place is still where it was found.
    std::ranges::sort(edits, std::ranges::greater{}, &Edit::from);
    QString result = *text;
    for (const Edit &edit : edits)
        result.replace(edit.from, edit.to - edit.from, edit.with);
    if (!wellFormed(result))
        return std::unexpected(Failure(Failure::Code::EpubPackageUnwritable));
    return (opf.startsWith(utf8Bom) ? utf8Bom.toByteArray() : QByteArray()) + result.toUtf8();
}
