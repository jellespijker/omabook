#pragma once

#include <QString>
#include <QStringView>

// Takes a reasoning model's thinking out of an answer as it streams. Some
// servers send it in the answer itself, between <think> and </think> (or
// <thinking> and </thinking>), rather than apart. A tag may arrive split
// over pieces, so what could still be the start of one is held back until
// it is plain whether it is; nothing inside one is ever passed on, and a
// tag still open when the answer ends is dropped with what it holds.
class ThinkingFilter {
public:
    // What of this piece may be shown now; empty while it is all thinking
    // or held back.
    [[nodiscard]] QString take(QStringView piece);
    // What was held back, once the answer has ended.
    [[nodiscard]] QString finish();

private:
    // Each says whether there is more of what is pending to look at now.
    bool skipThinking(QString &shown);
    bool passOn(QString &shown);

    QString m_pending;
    QString m_closing;        // the tag that ends the thinking under way; empty outside one
    bool m_shown = false;     // whether anything has been passed on yet
    bool m_skipSpace = false; // dropping the lines between the thinking and the answer
};
