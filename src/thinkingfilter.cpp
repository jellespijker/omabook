#include "thinkingfilter.h"

#include <array>

namespace {

using namespace Qt::StringLiterals;

constexpr std::array opening{"<think>"_L1, "<thinking>"_L1};

// Whether `text` is a tag that is not yet whole: the start of one, cut off.
bool startOf(QStringView text, QLatin1StringView tag) { return text.size() < tag.size() && tag.startsWith(text); }

} // namespace

QString ThinkingFilter::take(QStringView piece) {
    m_pending += piece;
    QString shown;
    while (!m_pending.isEmpty() && (m_closing.isEmpty() ? passOn(shown) : skipThinking(shown))) {
    }
    m_shown = m_shown || !shown.isEmpty();
    return shown;
}

bool ThinkingFilter::skipThinking(QString &shown) {
    const qsizetype end = m_pending.indexOf(m_closing);
    if (end < 0) {
        // Keep only what could be the start of the closing tag.
        qsizetype keep = qMin(m_pending.size(), m_closing.size() - 1);
        while (keep > 0 && !m_closing.startsWith(QStringView(m_pending).last(keep)))
            --keep;
        m_pending = m_pending.right(keep);
        return false;
    }
    m_pending.remove(0, end + m_closing.size());
    m_closing.clear();
    // The answer proper begins after the thinking, without the lines between.
    if (!m_shown && shown.trimmed().isEmpty()) {
        shown.clear();
        m_skipSpace = true;
    }
    return true;
}

bool ThinkingFilter::passOn(QString &shown) {
    // The lines between the thinking and the answer may come in later pieces.
    if (m_skipSpace) {
        while (!m_pending.isEmpty() && m_pending.front().isSpace())
            m_pending.remove(0, 1);
        if (m_pending.isEmpty())
            return false;
        m_skipSpace = false;
    }
    for (qsizetype at = m_pending.indexOf('<'); at >= 0; at = m_pending.indexOf('<', at + 1)) {
        const QStringView rest = QStringView(m_pending).mid(at);
        for (const QLatin1StringView tag : opening) {
            if (rest.startsWith(tag)) {
                shown += m_pending.left(at);
                m_pending.remove(0, at + tag.size());
                m_closing = u"</"_s + tag.mid(1);
                return true;
            }
            if (startOf(rest, tag)) {
                // Held back until it is plain whether it is a tag.
                shown += m_pending.left(at);
                m_pending.remove(0, at);
                return false;
            }
        }
    }
    shown += m_pending;
    m_pending.clear();
    return false;
}

QString ThinkingFilter::finish() {
    // An open tag at the end is thinking that was never finished: dropped.
    QString rest = m_closing.isEmpty() ? m_pending : QString();
    m_pending.clear();
    m_closing.clear();
    return rest;
}
