#pragma once

#include <QChar>
#include <QString>
#include <QStringView>

// Takes the HTML-like tags some models wrap an answer in out of it as it
// streams (`<panel>…</panel>`, `<div><p>…</p></div>`, a stray `<span>`), so
// what is left is the plain text or Markdown asked for. Any tag goes, known
// or not; `<br>` becomes a line break, and a line that held only tags goes
// with them. Markdown stays as written: a code span or a fenced code block
// keeps every `<tag>` in it, and `<https://…>`, `a < b` or `<-` are no tags.
// A tag (or a run of backticks) may arrive split over pieces, so what could
// still be one is held back until it is plain whether it is.
class MarkupFilter {
public:
    // What of this piece may be shown now; empty while it is all held back.
    [[nodiscard]] QString take(QStringView piece);
    // What was held back, once the answer has ended; the filter is then
    // ready for the next answer.
    [[nodiscard]] QString finish();

private:
    // Each looks at the start of `rest` and says how much of it it used:
    // nothing when it has to wait for the next piece. `end` says none comes.
    void run(bool end);
    qsizetype prose(QStringView rest, bool end);
    qsizetype code(QStringView rest, bool end);
    // The tag `rest` starts with taken out: its length; 0 when it is no tag.
    qsizetype tag(QStringView rest, bool end);
    qsizetype inSpan(QStringView rest, bool end);
    qsizetype inFence(QStringView rest, bool end);
    qsizetype newline();
    void show(QStringView text);

    QString m_pending;        // what is held back
    QString m_shown;          // what this piece lets through
    QString m_indent;         // the line's leading space, until it is plain the line stays
    QChar m_fenceMark;        // ` or ~ of the fenced code block under way
    qsizetype m_fence = 0;    // the length of its fence; 0 outside one
    qsizetype m_span = 0;     // the backticks that opened the code span under way; 0 outside one
    bool m_lineStart = true;  // nothing but space on this line yet
    bool m_lineShown = false; // something shown on this line
    bool m_lineTag = false;   // a tag was taken out of this line
    bool m_any = false;       // something shown in the answer yet
};
