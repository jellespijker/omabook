#include <QFile>
#include <QHash>

#include "bookarchive.h"
#include "formatreaders.h"
#include "htmlclean.h"

namespace {

// FictionBook is XML with its own element names. It is rewritten once into
// plain HTML, a chapter per top-level section, and cleaned like any other
// chapter when the reader asks for it.
class Fb2Document : public Document {
public:
    bool load(const QString &path, QString *error) {
        QByteArray bytes;
        if (path.endsWith(".fb2", Qt::CaseInsensitive)) {
            QFile file(path);
            if (file.open(QIODevice::ReadOnly))
                bytes = file.readAll();
        } else {
            const Archive archive(path);
            for (const QString &name : archive.entries())
                if (bytes.isEmpty() && name.endsWith(".fb2", Qt::CaseInsensitive))
                    bytes = archive.read(name);
        }
        if (bytes.isEmpty()) {
            *error = "The file is empty or holds no FictionBook.";
            return false;
        }
        convert(tokenizeHtml(decodeText(bytes)));
        if (m_sections.isEmpty()) {
            *error = "This FictionBook has no text.";
            return false;
        }
        return true;
    }

    Kind kind() const override { return Flow; }
    int count() const override { return m_sections.size(); }
    BookMeta meta() const override { return m_meta; }
    QList<TocEntry> toc() const override { return m_toc; }
    QByteArray resource(const QString &name) const override { return m_binaries.value(name); }

    QString html(int section, int width) const override {
        if (section < 0 || section >= m_sections.size())
            return {};
        CleanOptions options;
        options.image = [&](const QString &source) {
            const QString name = source.mid(1);
            return m_binaries.contains(name) ? bookImageAttributes(name, m_binaries.value(name), width) : QString();
        };
        options.link = [&](const QString &target) {
            if (!target.startsWith('#'))
                return target.startsWith("http") ? target : QString();
            const int to = m_sectionOfId.value(target.mid(1), -1);
            return to < 0 ? QString() : QString("omabook:%1%2").arg(to).arg(target);
        };
        return cleanHtml(m_sections.at(section), options);
    }

private:
    void convert(const QList<HtmlToken> &tokens) {
        static const QHash<QString, QString> map{
            {"p", "p"}, {"subtitle", "h3"}, {"epigraph", "blockquote"}, {"cite", "blockquote"}, {"poem", "blockquote"},
            {"stanza", "p"}, {"emphasis", "i"}, {"strong", "b"}, {"strikethrough", "s"}, {"sub", "sub"},
            {"sup", "sup"}, {"code", "code"}, {"annotation", "div"}, {"table", "table"}, {"tr", "tr"},
            {"td", "td"}, {"th", "th"},
        };
        QStringList path;       // the elements we are inside
        QString current;        // the chapter being written
        QString field;          // the text of the metadata element being read
        QString titleText;      // the words of the title being read
        QString first, middle, last, nick;
        int bodies = 0, sectionDepth = 0, titleDepth = 0;
        bool inSectionTitle = false;

        auto within = [&](const char *name) { return path.contains(QLatin1String(name)); };
        auto finishSection = [&](const QString &fallbackTitle) {
            if (htmlToText(current).isEmpty() && !current.contains("<img")) {
                current.clear();
                return;
            }
            if (m_toc.isEmpty() || m_toc.last().section != m_sections.size())
                m_toc.append({fallbackTitle, int(m_sections.size()), {}, 0});
            m_sections << current;
            current.clear();
        };

        for (const HtmlToken &token : tokens) {
            const QString &name = token.name;
            if (token.type == HtmlToken::Text) {
                if (within("binary")) field += token.text;
                else if (bodies == 0) field += token.text;
                else {
                    current += token.text;
                    if (titleDepth > 0) titleText += token.text;
                }
                continue;
            }
            const bool open = token.type == HtmlToken::Open;
            if (open && !token.selfClosing) path << name;

            if (name == "binary") {
                if (open) field.clear();
                else m_binaries.insert(m_binaryId, QByteArray::fromBase64(field.toLatin1()));
                if (open) m_binaryId = token.attributes.value("id");
            } else if (name == "body") {
                if (open) {
                    ++bodies;
                    m_notes = token.attributes.value("name") == "notes";
                } else {
                    finishSection(m_notes ? "Notes" : m_meta.title);
                }
            } else if (bodies == 0) {
                // The description: only title-info speaks for the book itself.
                // Emphasis inside a paragraph is part of its text.
                static const QStringList inlines{"emphasis", "strong", "a", "strikethrough", "sub", "sup"};
                if (inlines.contains(name))
                    continue;
                if (open) {
                    field.clear();
                    if (name == "author") first = middle = last = nick = QString();
                    if (name == "sequence" && within("title-info")) {
                        m_meta.series = token.attributes.value("name");
                        m_meta.seriesIndex = token.attributes.value("number").toDouble();
                    }
                    if (name == "image" && within("coverpage"))
                        m_coverId = token.attributes.value("href").mid(1);
                } else {
                    const QString value = htmlToText(field).simplified();
                    const bool info = within("title-info");
                    if (name == "first-name") first = value;
                    else if (name == "middle-name") middle = value;
                    else if (name == "last-name") last = value;
                    else if (name == "nickname") nick = value;
                    else if (name == "author" && info) {
                        const QString full = QStringList{first, middle, last}.join(' ').simplified();
                        if (!full.isEmpty() || !nick.isEmpty()) m_meta.authors << (full.isEmpty() ? nick : full);
                    } else if (name == "book-title" && info) m_meta.title = value;
                    else if (name == "genre" && info && !value.isEmpty()) m_meta.tags << QString(value).replace('_', ' ');
                    else if (name == "lang" && info) m_meta.language = value;
                    else if (name == "p" && within("annotation") && info)
                        m_meta.description += (m_meta.description.isEmpty() ? "" : "\n\n") + value;
                    else if (name == "publisher" && within("publish-info")) m_meta.publisher = value;
                    else if (name == "isbn" && within("publish-info")) m_meta.isbn = value;
                    else if (name == "year" && within("publish-info")) m_meta.published = value;
                    field.clear();
                }
            } else if (name == "section") {
                // The main text has a chapter per top-level section; the notes
                // stay together, so a footnote link lands among its neighbors.
                if (open) {
                    if (sectionDepth++ == 0 && !m_notes) finishSection(m_meta.title);
                    const QString id = token.attributes.value("id");
                    if (!id.isEmpty()) {
                        m_sectionOfId.insert(id, m_sections.size());
                        current += "<div id=\"" + id.toHtmlEscaped() + "\">";
                    } else {
                        current += "<div>";
                    }
                } else {
                    current += "</div>";
                    --sectionDepth;
                }
            } else if (name == "title") {
                inSectionTitle = sectionDepth > 0;
                titleDepth += open ? 1 : -1;
                const QString heading = sectionDepth > 1 || m_notes ? "h3" : "h2";
                if (open) {
                    titleText.clear();
                    current += "<" + heading + ">";
                } else {
                    current += "</" + heading + ">";
                    if (inSectionTitle && sectionDepth == 1 && !m_notes)
                        m_toc.append({htmlToText(titleText).simplified(), int(m_sections.size()), {}, 0});
                }
            } else if (titleDepth > 0 && name == "p") {
                // A title's paragraphs are its lines.
                if (!open) {
                    current += "<br>";
                    titleText += " ";
                }
            } else if (name == "v") {
                if (!open) current += "<br>";
            } else if (name == "empty-line") {
                current += "<br>";
            } else if (name == "text-author") {
                current += open ? "<p style=\"text-align:right\"><i>" : "</i></p>";
            } else if (name == "image") {
                if (open) current += "<img src=\"" + token.attributes.value("href").toHtmlEscaped() + "\">";
            } else if (name == "a") {
                current += open ? "<a href=\"" + token.attributes.value("href").toHtmlEscaped() + "\">" : "</a>";
            } else if (map.contains(name)) {
                const QString id = open ? token.attributes.value("id") : QString();
                if (!id.isEmpty()) m_sectionOfId.insert(id, m_sections.size());
                current += open ? "<" + map.value(name) + (id.isEmpty() ? "" : " id=\"" + id.toHtmlEscaped() + "\"") + ">"
                                : "</" + map.value(name) + ">";
            }

            if (!open && !path.isEmpty()) {
                const qsizetype at = path.lastIndexOf(name);
                if (at >= 0) path.resize(at);
            }
        }
        m_meta.tags.removeDuplicates();
        if (m_binaries.contains(m_coverId))
            m_meta.cover = QImage::fromData(m_binaries.value(m_coverId));
        for (TocEntry &entry : m_toc)
            if (entry.title.isEmpty())
                entry.title = QString::number(entry.section + 1);
    }

    QStringList m_sections;
    QList<TocEntry> m_toc;
    QHash<QString, QByteArray> m_binaries;
    QHash<QString, int> m_sectionOfId;
    QString m_binaryId, m_coverId;
    bool m_notes = false;
    BookMeta m_meta;
};

} // namespace

std::unique_ptr<Document> openFb2(const QString &path, QString *error) {
    auto document = std::make_unique<Fb2Document>();
    if (!document->load(path, error))
        return nullptr;
    return document;
}
