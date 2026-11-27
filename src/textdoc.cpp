#include <QFile>
#include <QRegularExpression>
#include <QTextDocument>
#include <algorithm>

#include "formatreaders.h"
#include "htmlclean.h"

namespace {

// Whether what was decoded is words rather than a file of something else:
// text has hardly a control character beside its tabs and line ends (and the
// escapes and end mark old text files have), and is more than empty bytes (#264).
[[nodiscard]] bool isText(const QString &text) {
    const auto odd = [](QChar letter) {
        const char16_t code = letter.unicode();
        return code > 0 && code < 0x20 && !letter.isSpace() && code != 0x1a && code != 0x1b;
    };
    const bool empty = !text.isEmpty() && std::ranges::all_of(text, [](QChar letter) { return letter.isNull(); });
    return !empty && std::ranges::count_if(text, odd) * 20 <= text.size();
}

// Plain text, Markdown and single HTML files: books with no container. Each
// is turned into HTML chapters once and cleaned when the reader asks.
class TextDocument : public Document {
public:
    std::expected<void, Failure> load(const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return std::unexpected(Failure(Failure::Code::FileUnreadable, {file.errorString()}));
        const QString text = decodeText(file.readAll());
        if (!isText(text))
            return std::unexpected(Failure(Failure::Code::FileNotText));
        const QString lower = path.toLower();
        if (lower.endsWith(".md") || lower.endsWith(".markdown")) {
            QTextDocument markdown;
            markdown.setMarkdown(text);
            m_sections << markdown.toHtml();
        } else if (lower.endsWith(".txt") || lower.endsWith(".text")) {
            fromPlainText(text);
        } else {
            static const QRegularExpression title("<title[^>]*>(.*?)</title>", QRegularExpression::CaseInsensitiveOption
                                                                                    | QRegularExpression::DotMatchesEverythingOption);
            m_meta.title = htmlToText(title.match(text).captured(1)).simplified();
            m_sections << text;
        }
        if (m_sections.isEmpty())
            return std::unexpected(Failure(Failure::Code::FileWithoutText));
        return {};
    }

    Kind kind() const override { return Flow; }
    int count() const override { return m_sections.size(); }
    BookMeta meta() const override { return m_meta; }
    QList<TocEntry> toc() const override { return m_toc; }

    QString html(int section, int) const override {
        if (section < 0 || section >= m_sections.size())
            return {};
        CleanOptions options;
        options.link = [](const QString &target) { return target.startsWith("http") ? target : QString(); };
        return cleanHtml(m_sections.at(section), options);
    }

private:
    // Paragraphs are separated by blank lines, and lines inside one are a
    // typewriter's wrapping. A long text is cut into chapters at its own
    // chapter headings, or failing that every so many words, so no chapter is
    // too long to lay out at once.
    void fromPlainText(QString text) {
        text.replace("\r\n", "\n").replace('\r', '\n');
        static const QRegularExpression blank("\\n[ \\t]*\\n");
        static const QRegularExpression heading("^(chapter|part|book|prologue|epilogue)\\b.{0,60}$",
                                                QRegularExpression::CaseInsensitiveOption);
        QString current;
        qsizetype length = 0;
        const auto finish = [this, &current, &length] {
            if (!current.isEmpty())
                m_sections << current;
            current.clear();
            length = 0;
        };
        for (const QString &paragraph : text.split(blank, Qt::SkipEmptyParts)) {
            const QString line = paragraph.simplified();
            if (line.isEmpty())
                continue;
            if (!paragraph.trimmed().contains('\n') && heading.match(line).hasMatch()) {
                if (length > 2000)
                    finish();
                m_toc.append({.title = line, .section = int(m_sections.size()), .anchor = {}, .level = 0});
                current += "<h2>" + line.toHtmlEscaped() + "</h2>";
            } else {
                if (length > 40000)
                    finish();
                current += "<p>" + line.toHtmlEscaped() + "</p>";
            }
            length += line.size();
        }
        finish();
    }

    QStringList m_sections;
    QList<TocEntry> m_toc;
    BookMeta m_meta;
};

} // namespace

OpenedDocument openText(const QString &path) { return loaded(std::make_unique<TextDocument>(), path); }
