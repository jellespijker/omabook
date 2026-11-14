#include "htmlclean.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <QTextDocumentFragment>

namespace {

bool nameStart(QChar c) { return c.isLetter() || c == '_'; }
bool nameChar(QChar c) { return c.isLetterOrNumber() || c == '_' || c == '-' || c == ':' || c == '.'; }

QString localName(const QString &name) { return name.section(':', -1).toLower(); }

// The few entities that matter inside an attribute; text keeps its entities,
// since rich text reads them itself.
QString decodeAttribute(QString value) {
    if (!value.contains('&'))
        return value;
    static const QRegularExpression numeric("&#(x?)([0-9a-fA-F]+);");
    for (auto match = numeric.match(value); match.hasMatch(); match = numeric.match(value)) {
        const uint code = match.captured(2).toUInt(nullptr, match.captured(1).isEmpty() ? 10 : 16);
        value.replace(match.capturedStart(), match.capturedLength(), QString::fromUcs4(reinterpret_cast<const char32_t *>(&code), 1));
    }
    return value.replace("&quot;", "\"").replace("&apos;", "'").replace("&lt;", "<").replace("&gt;", ">")
        .replace("&amp;", "&");
}

int styleFlags(const QString &declarations) {
    static const QRegularExpression italic("font-style\\s*:\\s*(italic|oblique)", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression bold("font-weight\\s*:\\s*(bold|bolder|[6-9]00)", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression center("text-align\\s*:\\s*center", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression right("text-align\\s*:\\s*(right|end)", QRegularExpression::CaseInsensitiveOption);
    int flags = 0;
    if (italic.match(declarations).hasMatch()) flags |= Italic;
    if (bold.match(declarations).hasMatch()) flags |= Bold;
    if (center.match(declarations).hasMatch()) flags |= Center;
    else if (right.match(declarations).hasMatch()) flags |= Right;
    return flags;
}

// What each element becomes; an element that isn't here leaves only its content.
const QHash<QString, QString> &elementMap() {
    static const QHash<QString, QString> map{
        {"p", "p"}, {"h1", "h1"}, {"h2", "h2"}, {"h3", "h3"}, {"h4", "h4"}, {"h5", "h5"}, {"h6", "h6"},
        {"b", "b"}, {"strong", "b"}, {"i", "i"}, {"em", "i"}, {"cite", "i"}, {"dfn", "i"}, {"var", "i"},
        {"u", "u"}, {"ins", "u"}, {"s", "s"}, {"strike", "s"}, {"del", "s"}, {"sub", "sub"}, {"sup", "sup"},
        {"small", "small"}, {"big", "big"}, {"code", "code"}, {"tt", "code"}, {"kbd", "code"}, {"samp", "code"},
        {"pre", "pre"}, {"blockquote", "blockquote"}, {"ul", "ul"}, {"ol", "ol"}, {"li", "li"}, {"dl", "dl"},
        {"dt", "dt"}, {"dd", "dd"}, {"table", "table"}, {"tr", "tr"}, {"td", "td"}, {"th", "th"},
        {"center", "center"}, {"div", "div"}, {"section", "div"}, {"article", "div"}, {"aside", "div"},
        {"header", "div"}, {"footer", "div"}, {"main", "div"}, {"figure", "div"}, {"nav", "div"},
        {"address", "div"}, {"figcaption", "p"}, {"caption", "p"},
    };
    return map;
}

const QSet<QString> &skippedElements() {
    static const QSet<QString> skipped{"script", "style", "head", "template", "iframe", "object", "audio", "video",
                                       "form", "button", "select", "textarea"};
    return skipped;
}

bool isBlock(const QString &tag) {
    static const QSet<QString> blocks{"p", "h1", "h2", "h3", "h4", "h5", "h6", "div", "blockquote", "ul", "ol",
                                      "li", "dl", "dt", "dd", "table", "tr", "td", "th", "pre", "center"};
    return blocks.contains(tag);
}

bool takesAlignment(const QString &tag) {
    return tag == "p" || tag == "div" || (tag.size() == 2 && tag[0] == 'h' && tag[1].isDigit());
}

// Elements that hold other elements, not text, so an anchor goes before them.
bool isContainer(const QString &tag) {
    return tag == "table" || tag == "tr" || tag == "ul" || tag == "ol" || tag == "dl";
}

} // namespace

QList<HtmlToken> tokenizeHtml(const QString &html) {
    QList<HtmlToken> tokens;
    const qsizetype size = html.size();
    qsizetype i = 0;
    auto addText = [&](const QString &text) {
        if (text.isEmpty())
            return;
        if (!tokens.isEmpty() && tokens.last().type == HtmlToken::Text)
            tokens.last().text += text;
        else
            tokens.append({HtmlToken::Text, {}, {}, text, false});
    };
    while (i < size) {
        const qsizetype open = html.indexOf('<', i);
        if (open < 0) {
            addText(html.mid(i));
            break;
        }
        addText(html.mid(i, open - i));
        i = open;
        if (html.mid(i, 4) == QLatin1String("<!--")) {
            const qsizetype end = html.indexOf("-->", i + 4);
            i = end < 0 ? size : end + 3;
        } else if (html.mid(i, 9) == QLatin1String("<![CDATA[")) {
            const qsizetype end = html.indexOf("]]>", i + 9);
            addText(html.mid(i + 9, (end < 0 ? size : end) - i - 9).toHtmlEscaped());
            i = end < 0 ? size : end + 3;
        } else if (i + 1 < size && (html[i + 1] == '!' || html[i + 1] == '?')) {
            const qsizetype end = html.indexOf('>', i);
            i = end < 0 ? size : end + 1;
        } else {
            qsizetype j = i + 1;
            const bool closing = j < size && html[j] == '/';
            if (closing)
                ++j;
            if (j >= size || !nameStart(html[j])) {
                // A stray "<" in the text, as in "a < b".
                addText("&lt;");
                ++i;
                continue;
            }
            const qsizetype nameAt = j;
            while (j < size && nameChar(html[j]))
                ++j;
            HtmlToken token;
            token.type = closing ? HtmlToken::Close : HtmlToken::Open;
            token.name = localName(html.mid(nameAt, j - nameAt));
            while (j < size && html[j] != '>') {
                if (html[j].isSpace()) {
                    ++j;
                } else if (html[j] == '/') {
                    token.selfClosing = true;
                    ++j;
                } else {
                    const qsizetype attributeAt = j;
                    while (j < size && !html[j].isSpace() && html[j] != '=' && html[j] != '>' && html[j] != '/')
                        ++j;
                    const QString attribute = localName(html.mid(attributeAt, j - attributeAt));
                    while (j < size && html[j].isSpace())
                        ++j;
                    QString value;
                    if (j < size && html[j] == '=') {
                        ++j;
                        while (j < size && html[j].isSpace())
                            ++j;
                        if (j < size && (html[j] == '"' || html[j] == '\'')) {
                            const QChar quote = html[j];
                            const qsizetype end = html.indexOf(quote, j + 1);
                            value = html.mid(j + 1, (end < 0 ? size : end) - j - 1);
                            j = end < 0 ? size : end + 1;
                        } else {
                            const qsizetype valueAt = j;
                            while (j < size && !html[j].isSpace() && html[j] != '>')
                                ++j;
                            value = html.mid(valueAt, j - valueAt);
                        }
                    }
                    if (!attribute.isEmpty() && !token.attributes.contains(attribute))
                        token.attributes.insert(attribute, decodeAttribute(value));
                    else if (attribute.isEmpty())
                        ++j;
                }
            }
            i = j < size ? j + 1 : size;
            tokens.append(token);
            // A script's or stylesheet's content isn't markup: take it whole.
            if (!closing && !token.selfClosing && (token.name == "script" || token.name == "style")) {
                const qsizetype end = html.indexOf("</" + token.name, i, Qt::CaseInsensitive);
                tokens.append({HtmlToken::Text, {}, {}, html.mid(i, (end < 0 ? size : end) - i), false});
                i = end < 0 ? size : end;
            }
        }
    }
    return tokens;
}

QHash<QString, int> classStyles(const QString &css) {
    QHash<QString, int> styles;
    static const QRegularExpression rule("([^{}]+)\\{([^{}]*)\\}");
    // A class on its own or on an element (".it", "p.it"); a class that styles
    // its descendants would need the cascade, which rich text doesn't have.
    static const QRegularExpression simple("^[A-Za-z0-9]*\\.([A-Za-z0-9_-]+)$");
    for (auto it = rule.globalMatch(css); it.hasNext();) {
        const auto match = it.next();
        const int flags = styleFlags(match.captured(2));
        if (!flags)
            continue;
        for (const QString &selector : match.captured(1).split(',')) {
            const auto name = simple.match(selector.trimmed());
            if (name.hasMatch())
                styles[name.captured(1)] |= flags;
        }
    }
    return styles;
}

QString cleanHtml(const QString &html, const CleanOptions &options) {
    struct Open {
        QString name;
        QString closers;
    };
    QList<Open> stack;
    QString out;
    out.reserve(html.size());
    QString skipping;
    int skipDepth = 0;

    auto closeTo = [&](qsizetype index) {
        while (stack.size() > index)
            out += stack.takeLast().closers;
    };

    for (const HtmlToken &token : tokenizeHtml(html)) {
        if (skipDepth > 0) {
            if (token.name == skipping && token.type == HtmlToken::Open && !token.selfClosing)
                ++skipDepth;
            else if (token.name == skipping && token.type == HtmlToken::Close)
                --skipDepth;
            continue;
        }
        if (token.type == HtmlToken::Text) {
            out += token.text;
            continue;
        }
        const QString &name = token.name;
        if (token.type == HtmlToken::Close) {
            for (qsizetype index = stack.size() - 1; index >= 0; --index) {
                if (stack[index].name == name) {
                    closeTo(index);
                    break;
                }
            }
            continue;
        }
        if (skippedElements().contains(name)) {
            if (!token.selfClosing) {
                skipping = name;
                skipDepth = 1;
            }
            continue;
        }

        QString identifier = token.attributes.value("id");
        if (identifier.isEmpty() && name == "a")
            identifier = token.attributes.value("name");
        const QString anchor = identifier.isEmpty() ? QString()
                                                    : "<a name=\"" + identifier.toHtmlEscaped() + "\"></a>";

        if (name == "br" || name == "hr") {
            out += anchor + "<" + name + ">";
            continue;
        }
        if (name == "img" || name == "image") {
            QString source = token.attributes.value("src");
            if (source.isEmpty())
                source = token.attributes.value("href");
            const QString attributes = options.image && !source.isEmpty() ? options.image(source) : QString();
            out += anchor;
            if (!attributes.isEmpty())
                out += "<img " + attributes + ">";
            continue;
        }

        int flags = styleFlags(token.attributes.value("style"));
        for (const QString &className : token.attributes.value("class").split(' ', Qt::SkipEmptyParts))
            flags |= options.styles.value(className);

        const QString tag = elementMap().value(name);
        // HTML lets a paragraph end where the next block begins.
        if (isBlock(tag) && !stack.isEmpty() && stack.last().name == "p")
            closeTo(stack.size() - 1);

        QString opening, closers;
        if (name == "a") {
            const QString target = token.attributes.value("href");
            const QString link = options.link && !target.isEmpty() ? options.link(target) : QString();
            if (!link.isEmpty()) {
                opening = "<a href=\"" + link.toHtmlEscaped() + "\">";
                closers = "</a>";
            }
            opening = anchor + opening;
        } else if (!tag.isEmpty()) {
            QString alignment;
            if (takesAlignment(tag) && (flags & (Center | Right)))
                alignment = flags & Center ? " align=\"center\"" : " align=\"right\"";
            const QString element = "<" + tag + alignment + ">";
            opening = isContainer(tag) ? anchor + element : element + anchor;
            closers = "</" + tag + ">";
        } else {
            opening = anchor;
        }
        if (flags & Bold) {
            opening += "<b>";
            closers.prepend("</b>");
        }
        if (flags & Italic) {
            opening += "<i>";
            closers.prepend("</i>");
        }
        out += opening;
        if (token.selfClosing)
            out += closers;
        else
            stack.append({name, closers});
    }
    closeTo(0);
    return out;
}

QString htmlToText(const QString &html) {
    QString text = QTextDocumentFragment::fromHtml(html).toPlainText();
    // Pictures become object replacement characters, and breaks line separators.
    text.remove(QChar(0xFFFC));
    text.replace(QChar(0x2028), '\n');
    text.replace(QChar(0xA0), ' ');
    return text.trimmed();
}

QString decodeText(const QByteArray &bytes) {
    if (const auto encoding = QStringConverter::encodingForData(bytes)) {
        QStringDecoder decoder(*encoding);
        return decoder(bytes);
    }
    static const QRegularExpression declared("(?:encoding|charset)\\s*=\\s*[\"']?([A-Za-z0-9_-]+)",
                                             QRegularExpression::CaseInsensitiveOption);
    const auto match = declared.match(QString::fromLatin1(bytes.left(1024)));
    if (match.hasMatch() && match.captured(1).compare("utf-8", Qt::CaseInsensitive) != 0) {
        QStringDecoder decoder(match.captured(1).toLatin1().constData());
        if (decoder.isValid())
            return decoder(bytes);
    }
    QStringDecoder utf8(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
    const QString text = utf8(bytes);
    if (!utf8.hasError())
        return text;
    QStringDecoder western("windows-1252");
    return western.isValid() ? QString(western(bytes)) : QString::fromLatin1(bytes);
}
