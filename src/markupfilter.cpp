#include "markupfilter.h"

#include <utility>

namespace {

using namespace Qt::StringLiterals;

// Longer than this, what starts like a tag is text.
constexpr qsizetype longestTag = 300;
// What waiting on the next piece is said with.
constexpr qsizetype wait = -1;

bool letter(QChar c) { return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z'); }

// How many times the first character of `text` repeats from its start.
qsizetype runOf(QStringView text) {
    qsizetype n = 1;
    while (n < text.size() && text[n] == text[0])
        ++n;
    return n;
}

bool inName(QChar c) { return letter(c) || c.isDigit() || c == u'-'; }

bool afterName(QChar c) { return c == u'>' || c == u'/' || c == u' ' || c == u'\t'; }

// Where the tag whose name ends at `from` ends, past its `>`: 0 when it
// does not, `undecided` when what is to come may still end it.
qsizetype tagEnd(QStringView text, qsizetype from, qsizetype undecided) {
    const qsizetype last = qMin(text.size(), longestTag);
    for (qsizetype at = from; at < last; ++at) {
        if (text[at] == u'>')
            return at + 1;
        if (text[at] == u'<' || text[at] == u'\n')
            return 0;
    }
    return last == longestTag ? 0 : undecided;
}

// The length of the tag `text` starts with ('<' first): `<name…>` or
// `</name…>`, the name a letter and letters, digits or dashes, followed by
// `>`, `/` or a space. 0 when it is no tag; `wait` when the rest may still
// make it one.
qsizetype tagAt(QStringView text, bool end) {
    const qsizetype undecided = end ? 0 : wait;
    qsizetype at = text.startsWith(u"</") ? 2 : 1;
    if (at >= text.size())
        return undecided;
    if (!letter(text[at]))
        return 0;
    while (at < text.size() && inName(text[at]))
        ++at;
    if (at >= text.size())
        return undecided;
    return afterName(text[at]) ? tagEnd(text, at, undecided) : 0;
}

bool isBreak(QStringView tag) { return tag.startsWith("<br"_L1, Qt::CaseInsensitive) && !letter(tag[3]) && !tag[3].isDigit(); }

} // namespace

QString MarkupFilter::take(QStringView piece) {
    m_pending += piece;
    run(false);
    return std::exchange(m_shown, {});
}

QString MarkupFilter::finish() {
    run(true);
    QString rest = std::exchange(m_shown, {});
    *this = {};
    return rest;
}

void MarkupFilter::run(bool end) {
    qsizetype at = 0;
    while (at < m_pending.size()) {
        const QStringView rest = QStringView(m_pending).mid(at);
        qsizetype used = 0;
        if (m_fence > 0)
            used = inFence(rest, end);
        else if (m_span > 0)
            used = inSpan(rest, end);
        else
            used = prose(rest, end);
        if (used <= 0)
            break;
        at += used;
    }
    m_pending.remove(0, at);
}

qsizetype MarkupFilter::prose(QStringView rest, bool end) {
    const QChar c = rest.front();
    if (c == u'\n')
        return newline();
    if (m_lineStart && (c == u' ' || c == u'\t')) {
        m_indent += c;
        return 1;
    }
    if (c == u'`' || (c == u'~' && m_lineStart))
        return code(rest, end);
    if (c == u'<') {
        if (const qsizetype used = tag(rest, end); used != 0)
            return used;
    }
    show(rest.first(1));
    return 1;
}

qsizetype MarkupFilter::code(QStringView rest, bool end) {
    // A run of backticks or tildes: a fence at a line's start, else a code span's start.
    const QChar c = rest.front();
    const qsizetype n = runOf(rest);
    if (n == rest.size() && !end)
        return wait;
    if (m_lineStart && n >= 3) {
        m_fenceMark = c;
        m_fence = n;
    } else if (c == u'`') {
        m_span = n;
    }
    show(rest.first(n));
    return n;
}

qsizetype MarkupFilter::tag(QStringView rest, bool end) {
    const qsizetype length = tagAt(rest, end);
    if (length <= 0)
        return length;
    // What indents a tag is the markup's layout, not the answer's.
    if (m_lineStart)
        m_indent.clear();
    // A break is a line's end: the newline that may follow it is not another.
    if (isBreak(rest.first(length)))
        show(u"\n");
    m_lineTag = true;
    return length;
}

qsizetype MarkupFilter::inSpan(QStringView rest, bool end) {
    // A code span ends with as many backticks as began it, or with its line.
    if (rest.front() == u'\n') {
        m_span = 0;
        return newline();
    }
    const qsizetype n = rest.front() == u'`' ? runOf(rest) : 1;
    if (rest.front() == u'`' && n == rest.size() && !end)
        return wait;
    if (rest.front() == u'`' && n == m_span)
        m_span = 0;
    show(rest.first(n));
    return n;
}

qsizetype MarkupFilter::inFence(QStringView rest, bool end) {
    // A fenced code block ends with a fence at least as long, at a line's start.
    const bool fence = m_lineStart && rest.front() == m_fenceMark;
    const qsizetype n = fence ? runOf(rest) : 1;
    if (fence && n == rest.size() && !end)
        return wait;
    if (fence && n >= m_fence)
        m_fence = 0;
    show(rest.first(n));
    return n;
}

qsizetype MarkupFilter::newline() {
    // A line that held only tags goes with them, as do blank lines before the answer.
    if ((m_lineTag && !m_lineShown) || !m_any)
        m_indent.clear();
    else
        show(u"\n");
    m_lineStart = true;
    m_lineShown = m_lineTag = false;
    return 1;
}

void MarkupFilter::show(QStringView text) {
    m_shown += std::exchange(m_indent, {});
    m_shown += text;
    for (const QChar c : text) {
        if (c == u'\n') {
            m_lineStart = true;
            m_lineShown = m_lineTag = false;
        } else if (!c.isSpace()) {
            m_lineStart = false;
            m_lineShown = m_any = true;
        }
    }
}
