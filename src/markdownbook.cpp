#include <QFileInfo>
#include <QRegularExpression>
#include <QTextDocument>
#include <algorithm>

#include "formatprovider.h"

// Markdown books: a folder of Markdown files, or the same folder zipped. It
// is a format provider like any a plugin adds (see formatprovider.h), which
// is the whole of what it takes to add a format: say which files are yours,
// and describe the book in them as chapters.
//
// The chapters are the Markdown files. With a SUMMARY.md (the contents file
// of mdBook and GitBook) they come in its order, under its titles and
// nesting; without one, README or index first and the rest by name. The
// book's details are read from the front matter of its first file, or from
// mdBook's book.toml.

namespace {

const QStringList markdownSuffixes{"md", "markdown", "mdown"};
const QStringList pictureSuffixes{"png", "jpg", "jpeg", "gif", "webp", "svg", "bmp"};

QString suffix(const QString &name) { return QFileInfo(name).suffix().toLower(); }
bool isMarkdown(const QString &name) { return markdownSuffixes.contains(suffix(name)); }
QString baseName(const QString &name) { return QFileInfo(name).completeBaseName(); }
int depth(const QString &name) { return int(name.count('/')); }

// The book is Markdown and what Markdown brings along: its pictures and the
// odd settings or licence file. Anything else (source code, other books)
// makes the folder something other than a book of this kind.
bool claims(const BookSource &source) {
    static const QStringList beside{"css", "yml", "yaml", "toml", "json", "txt", ""};
    const QStringList entries = source.entries();
    const auto fits = [](const QString &name) {
        return isMarkdown(name) || pictureSuffixes.contains(suffix(name)) || beside.contains(suffix(name));
    };
    return std::any_of(entries.begin(), entries.end(), isMarkdown) && std::all_of(entries.begin(), entries.end(), fits);
}

QString unquoted(QString value) {
    value = value.trimmed();
    if (value.size() >= 2 && (value.startsWith('"') || value.startsWith('\'')) && value.endsWith(value.at(0)))
        value = value.mid(1, value.size() - 2);
    return value;
}

// "[a, b]" or "a, b" as its parts.
QStringList listed(QString value) {
    value = value.trimmed();
    if (value.startsWith('[') && value.endsWith(']'))
        value = value.mid(1, value.size() - 2);
    QStringList parts;
    for (const QString &part : value.split(',', Qt::SkipEmptyParts))
        if (!unquoted(part).isEmpty())
            parts << unquoted(part);
    return parts;
}

// The simple "key: value" lines front matter and book.toml are made of, with
// a list either on the line or as "- item" lines under it. This is not a
// YAML parser: it reads what books carry, and skips what it doesn't know.
QHash<QString, QStringList> settings(const QString &text, QChar separator) {
    QHash<QString, QStringList> found;
    QString key;
    for (const QString &line : text.split('\n')) {
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith("- ") && !key.isEmpty()) {
            found[key] << unquoted(trimmed.mid(2));
        } else if (const qsizetype at = trimmed.indexOf(separator); at > 0 && !trimmed.startsWith('#') && !trimmed.startsWith('[')) {
            key = trimmed.left(at).trimmed().toLower();
            const QString value = trimmed.mid(at + 1).trimmed();
            if (!value.isEmpty())
                found[key] = value.startsWith('[') ? listed(value) : QStringList{unquoted(value)};
        }
    }
    return found;
}

// A file's text without its front matter, which is handed back in `front`.
QString withoutFrontMatter(QString text, QString *front = nullptr) {
    text.replace("\r\n", "\n");
    static const QRegularExpression block("\\A---\\n(.*?)\\n(---|\\.\\.\\.)[ \\t]*(\\n|\\z)", QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch match = block.match(text);
    if (!match.hasMatch())
        return text;
    if (front)
        *front = match.captured(1);
    return text.mid(match.capturedEnd());
}

QString firstHeading(const QString &markdown) {
    static const QRegularExpression heading("^#{1,6}[ \\t]+(.+?)[ \\t]*#*$", QRegularExpression::MultilineOption);
    static const QRegularExpression underlined("^(\\S.*)\\n(=+|-+)[ \\t]*$", QRegularExpression::MultilineOption);
    const QRegularExpressionMatch hashed = heading.match(markdown), lined = underlined.match(markdown);
    if (hashed.hasMatch() && (!lined.hasMatch() || hashed.capturedStart() < lined.capturedStart()))
        return hashed.captured(1).trimmed();
    return lined.hasMatch() ? lined.captured(1).trimmed() : QString();
}

// "02-the_road" as "The road": a file's name where it has no heading.
QString titleFromName(const QString &name) {
    QString title = baseName(name);
    static const QRegularExpression number("^\\d+[-_. ]+");
    title.remove(number);
    title.replace('_', ' ').replace('-', ' ');
    title = title.simplified();
    return title.isEmpty() ? baseName(name) : title.left(1).toUpper() + title.mid(1);
}

struct Planned {
    QString file, title;
    int level = 0;
};

// The chapters in order: as SUMMARY.md lists them, or by name.
QList<Planned> plan(const BookSource &source, const QStringList &entries) {
    QList<Planned> chapters;
    QStringList summaries;
    for (const QString &name : entries)
        if (QFileInfo(name).fileName().compare("SUMMARY.md", Qt::CaseInsensitive) == 0)
            summaries << name;
    std::sort(summaries.begin(), summaries.end(), [](const QString &a, const QString &b) { return depth(a) < depth(b); });
    if (!summaries.isEmpty()) {
        static const QRegularExpression item("^([ \\t]*)(?:[-*+][ \\t]+)?\\[([^\\]]+)\\]\\(([^)\\s]*)\\)");
        QList<int> indents;
        for (const QString &line : withoutFrontMatter(source.text(summaries.first())).split('\n')) {
            const QRegularExpressionMatch match = item.match(line);
            const QString file = match.hasMatch() ? BookSource::resolve(summaries.first(), match.captured(3)) : QString();
            if (file.isEmpty() || !entries.contains(file) || !isMarkdown(file))
                continue;
            // Nesting is by how far a line is indented, whatever the width.
            const int indent = int(match.captured(1).replace('\t', "    ").size());
            while (!indents.isEmpty() && indents.last() > indent)
                indents.removeLast();
            if (indents.isEmpty() || indents.last() < indent)
                indents << indent;
            chapters.append({file, match.captured(2).trimmed(), int(indents.size()) - 1});
        }
        if (!chapters.isEmpty())
            return chapters;
    }
    // No contents file: README or index opens the book, the rest follow by
    // name, and a file in a folder sits under it in the contents.
    QStringList files;
    for (const QString &name : entries)
        if (isMarkdown(name))
            files << name;
    const auto opens = [](const QString &name) {
        const QString base = baseName(name).toLower();
        return depth(name) == 0 && (base == "readme" || base == "index");
    };
    std::stable_partition(files.begin(), files.end(), opens);
    const int shallowest = files.isEmpty() ? 0 : depth(*std::min_element(files.begin(), files.end(), [](const QString &a, const QString &b) {
        return depth(a) < depth(b);
    }));
    for (const QString &file : std::as_const(files))
        chapters.append({file, {}, depth(file) - shallowest});
    return chapters;
}

std::optional<BookDescription> describe(const BookSource &source, QString *error) {
    const QStringList entries = source.entries();
    const QList<Planned> chapters = plan(source, entries);
    if (chapters.isEmpty()) {
        if (error)
            *error = "There are no Markdown files in " + QFileInfo(source.path()).fileName() + ".";
        return std::nullopt;
    }

    BookDescription book;
    QString front;
    for (const Planned &planned : chapters) {
        QString chapterFront;
        const QString markdown = withoutFrontMatter(source.text(planned.file), &chapterFront);
        if (front.isEmpty())
            front = chapterFront;
        QTextDocument converted;
        converted.setMarkdown(markdown);
        QString title = planned.title.isEmpty() ? firstHeading(markdown) : planned.title;
        book.chapters.append({title.isEmpty() ? titleFromName(planned.file) : title, converted.toHtml(), planned.file, planned.level});
    }

    // What the book says of itself: front matter first, mdBook's book.toml
    // where there is none, and the first heading or the folder's name last.
    QHash<QString, QStringList> said = settings(front, ':');
    for (const QString &name : entries)
        if (said.isEmpty() && QFileInfo(name).fileName() == "book.toml")
            said = settings(source.text(name), '=');
    const auto one = [&](std::initializer_list<const char *> keys) {
        for (const char *key : keys)
            if (said.contains(key))
                return said.value(key).join(", ");
        return QString();
    };
    const auto many = [&](std::initializer_list<const char *> keys) {
        for (const char *key : keys)
            if (said.contains(key))
                return said.value(key).size() == 1 ? listed(said.value(key).first()) : said.value(key);
        return QStringList();
    };
    book.meta.title = one({"title"});
    if (book.meta.title.isEmpty())
        book.meta.title = firstHeading(withoutFrontMatter(source.text(chapters.first().file)));
    if (book.meta.title.isEmpty())
        book.meta.title = titleFromName(source.name());
    book.meta.authors = many({"authors", "author"});
    book.meta.language = one({"language", "lang"});
    book.meta.description = one({"description", "summary"});
    book.meta.tags = many({"tags", "keywords", "subjects"});
    book.meta.series = one({"series"});
    book.meta.seriesIndex = one({"series_index", "series-index", "volume"}).toDouble();
    book.meta.publisher = one({"publisher"});
    book.meta.published = one({"date", "published"});
    book.meta.isbn = one({"isbn"});

    // The cover the book names, or a picture called "cover" at its top.
    book.cover = BookSource::resolve(chapters.first().file, one({"cover", "cover-image", "cover_image"}));
    for (const QString &name : entries)
        if (book.cover.isEmpty() && depth(name) <= 1 && baseName(name).toLower() == "cover" && pictureSuffixes.contains(suffix(name)))
            book.cover = name;
    if (!entries.contains(book.cover))
        book.cover.clear();
    return book;
}

} // namespace

FormatProvider markdownBookProvider() {
    return {"markdown", "Markdown book", {"mdz"}, claims, describe};
}
