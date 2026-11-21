#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>

class Reader;

// The words the reader has selected, as the plugins are told of them (#137):
// a moment after the selection stops changing, it is made a passage of the
// open book (Passage, ADR 0004), its words placed in the book's own text
// nearest where the reader is, and handed to the plugins
// (ScriptPlugins::setSelection), which answer it to `omabook.reader.selection()`
// and tell `reader.selected`. A selection cleared is told as none.
class ReadingSelection : public QObject {
    Q_OBJECT

public:
    explicit ReadingSelection(Reader *reader);

    // The words selected on the page now, "" for none: what the page says as it changes.
    Q_INVOKABLE void changed(const QString &words);
    // The passage the words are, {book, global_id, passage, confidence}, or
    // empty when there are none or the book has no text to find them in.
    [[nodiscard]] QJsonObject passage(const QString &words) const;

private:
    void tell();

    Reader *m_reader;
    QString m_words;
    QTimer m_settle;
};
