#include <QFileInfo>
#include <QRegularExpression>
#include <QTextDocument>
#include <algorithm>
#include <array>
#include <ranges>

#include "formatprovider.h"

using namespace Qt::StringLiterals;

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

constexpr std::array markdownSuffixes{"md"_L1, "markdown"_L1, "mdown"_L1};
constexpr std::array pictureSuffixes{"png"_L1, "jpg"_L1, "jpeg"_L1, "gif"_L1, "webp"_L1, "svg"_L1, "bmp"_L1};

[[nodiscard]] QString suffix(const QString &name) { return QFileInfo(name).suffix().toLower(); }
[[nodiscard]] bool isMarkdown(const QString &name) { return std::ranges::contains(markdownSuffixes, suffix(name)); }
[[nodiscard]] bool isPicture(const QString &name) { return std::ranges::contains(pictureSuffixes, suffix(name)); }
[[nodiscard]] QString baseName(const QString &name) { return QFileInfo(name).completeBaseName(); }
[[nodiscard]] int depth(const QString &name) { return int(name.count('/')); }

// The book is Markdown and what Markdown brings along: its pictures and the
// odd settings or licence file. Anything else (source code, other books)
// makes the folder something other than a book of this kind.
bool claims(const BookSource &source) {
    static constexpr std::array beside{"css"_L1, "yml"_L1, "yaml"_L1, "toml"_L1, "json"_L1, "txt"_L1, ""_L1};
    const QStringList entries = source.entries();
    const auto fits = [](const QString &name) {
        return isMarkdown(name) || isPicture(name) || std::ranges::contains(beside, suffix(name));
    };
    return std::ranges::any_of(entries, isMarkdown) && std::ranges::all_of(entries, fits);
}

[[nodiscard]] QString unquoted(QString value) {
    value = value.trimmed();
    if (value.size() >= 2 && (value.startsWith('"') || value.startsWith('\'')) && value.endsWith(value.at(0)))
        value = value.mid(1, value.size() - 2);
    return value;
}

// "[a, b]" or "a, b" as its parts.
[[nodiscard]] QStringList listed(QString value) {
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
[[nodiscard]] QHash<QString, QStringList> settings(const QString &text, QChar separator) {
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

// A file's text, and the front matter that came before it.
struct Parted {
    QString front;
    QString text;
};

[[nodiscard]] Parted withoutFrontMatter(QString text) {
    text.replace("\r\n", "\n");
    static const QRegularExpression block("\\A---\\n(.*?)\\n(---|\\.\\.\\.)[ \\t]*(\\n|\\z)", QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch match = block.match(text);
    if (!match.hasMatch())
        return {.front = {}, .text = text};
    return {.front = match.captured(1), .text = text.mid(match.capturedEnd())};
}

[[nodiscard]] QString firstHeading(const QString &markdown) {
    static const QRegularExpression heading("^#{1,6}[ \\t]+(.+?)[ \\t]*#*$", QRegularExpression::MultilineOption);
    static const QRegularExpression underlined("^(\\S.*)\\n(=+|-+)[ \\t]*$", QRegularExpression::MultilineOption);
    const QRegularExpressionMatch hashed = heading.match(markdown), lined = underlined.match(markdown);
    if (hashed.hasMatch() && (!lined.hasMatch() || hashed.capturedStart() < lined.capturedStart()))
        return hashed.captured(1).trimmed();
    return lined.hasMatch() ? lined.captured(1).trimmed() : QString();
}

// "02-the_road" as "The road": a file's name where it has no heading.
[[nodiscard]] QString titleFromName(const QString &name) {
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
[[nodiscard]] QList<Planned> plan(const BookSource &source, const QStringList &entries) {
    QList<Planned> chapters;
    // The contents file nearest the top of the book is the book's.
    auto summaries = entries | std::views::filter([](const QString &name) {
        return QFileInfo(name).fileName().compare("SUMMARY.md"_L1, Qt::CaseInsensitive) == 0;
    });
    if (const auto summary = std::ranges::min_element(summaries, {}, depth); summary != summaries.end()) {
        static const QRegularExpression item("^([ \\t]*)(?:[-*+][ \\t]+)?\\[([^\\]]+)\\]\\(([^)\\s]*)\\)");
        QList<int> indents;
        for (const QString &line : withoutFrontMatter(source.text(*summary)).text.split('\n')) {
            const QRegularExpressionMatch match = item.match(line);
            const QString file = match.hasMatch() ? BookSource::resolve(*summary, match.captured(3)) : QString();
            if (file.isEmpty() || !entries.contains(file) || !isMarkdown(file))
                continue;
            // Nesting is by how far a line is indented, whatever the width.
            const int indent = int(match.captured(1).replace('\t', "    ").size());
            while (!indents.isEmpty() && indents.last() > indent)
                indents.removeLast();
            if (indents.isEmpty() || indents.last() < indent)
                indents << indent;
            chapters.append({.file = file, .title = match.captured(2).trimmed(), .level = int(indents.size()) - 1});
        }
        if (!chapters.isEmpty())
            return chapters;
    }
    // No contents file: README or index opens the book, the rest follow by
    // name, and a file in a folder sits under it in the contents.
    QStringList files = entries | std::views::filter(isMarkdown) | std::ranges::to<QStringList>();
    const auto opens = [](const QString &name) {
        const QString base = baseName(name).toLower();
        return depth(name) == 0 && (base == "readme"_L1 || base == "index"_L1);
    };
    std::ranges::stable_partition(files, opens);
    const int shallowest = files.isEmpty() ? 0 : std::ranges::min(files | std::views::transform(depth));
    for (const QString &file : std::as_const(files))
        chapters.append({.file = file, .title = {}, .level = depth(file) - shallowest});
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
        const auto [chapterFront, markdown] = withoutFrontMatter(source.text(planned.file));
        if (front.isEmpty())
            front = chapterFront;
        QTextDocument converted;
        converted.setMarkdown(markdown);
        const QString title = planned.title.isEmpty() ? firstHeading(markdown) : planned.title;
        book.chapters.append({.title = title.isEmpty() ? titleFromName(planned.file) : title,
                              .html = converted.toHtml(),
                              .file = planned.file,
                              .level = planned.level});
    }

    // What the book says of itself: front matter first, mdBook's book.toml
    // where there is none, and the first heading or the folder's name last.
    QHash<QString, QStringList> said = settings(front, ':');
    for (const QString &name : entries)
        if (said.isEmpty() && QFileInfo(name).fileName() == "book.toml")
            said = settings(source.text(name), '=');
    const auto one = [&said](std::initializer_list<const char *> keys) {
        for (const char *key : keys)
            if (said.contains(key))
                return said.value(key).join(", ");
        return QString();
    };
    const auto many = [&said](std::initializer_list<const char *> keys) {
        for (const char *key : keys)
            if (said.contains(key))
                return said.value(key).size() == 1 ? listed(said.value(key).first()) : said.value(key);
        return QStringList();
    };
    book.meta.title = one({"title"});
    if (book.meta.title.isEmpty())
        book.meta.title = firstHeading(withoutFrontMatter(source.text(chapters.first().file)).text);
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
        if (book.cover.isEmpty() && depth(name) <= 1 && baseName(name).toLower() == "cover"_L1 && isPicture(name))
            book.cover = name;
    if (!entries.contains(book.cover))
        book.cover.clear();
    return book;
}

} // namespace

FormatProvider markdownBookProvider() {
    return {.id = "markdown", .name = "Markdown book", .extensions = {"mdz"}, .claims = claims, .describe = describe};
}
