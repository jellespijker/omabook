#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QString>
#include <QVariantList>
#include <optional>
#include <utility>

#include "library.h"

class Reader;

// The open book's margin notes (#82), for the reading view: words the reader
// wrote about a passage, kept as an annotation of the kind "margin-note"
// with its passage (Passage, found by its words, ADR 0004) and the words in
// `note`. They travel between one's own computers in the change log as every
// annotation does, and are found again by their words when the book's file
// changes (ReadingPassages); one that can't be is lost, and listed here for
// the reader to place again or remove.
//
// The view asks where each note of the chapter it shows is in its text
// (`spans`), by the passage's words, so a note stands at its passage
// whatever the window, the type or the hyphenation; where they then stand
// in the margin column is PageLayout::stack's. Whether there is a margin
// column at all is the layout's (PageLayout::columns): this says only
// whether the book has notes that want one.
class MarginNotes : public QObject {
    Q_OBJECT
    // The reader whose open book's notes these are (the window's `reader`).
    Q_PROPERTY(QObject *reader READ reader WRITE setReader NOTIFY changed)
    // Its notes on a passage that is there, in the order of the book: [{id,
    // section, fraction, quote, note}]; and those that are lost, the same.
    Q_PROPERTY(QVariantList notes READ notes NOTIFY changed)
    Q_PROPERTY(QVariantList lost READ lost NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)
    // A note being written on a passage that has none yet, which the page
    // makes room for as for one that is there.
    Q_PROPERTY(bool drafting READ drafting WRITE setDrafting NOTIFY changed)
    // Changes with every write and every new chapter, for what is worked out from them.
    Q_PROPERTY(int revision READ revision NOTIFY changed)

public:
    explicit MarginNotes(QObject *parent = nullptr);

    QObject *reader() const;
    void setReader(QObject *reader);
    QVariantList notes() const;
    QVariantList lost() const;
    int count() const { return static_cast<int>(m_notes.size()); }
    bool drafting() const noexcept { return m_drafting; }
    void setDrafting(bool drafting);
    int revision() const noexcept { return m_revision; }

    // Where the notes of the chapter shown are in a text showing it (a
    // TextEdit's document): [{id, start, end, note}], in the order of their
    // passages; a note whose words it doesn't hold is left out.
    Q_INVOKABLE QVariantList spans(QQuickTextDocument *chapter);
    // A note with these words on the passage of the chapter shown from
    // character `start` to `end` of the document: its number, or 0 when
    // there are no words, no words there, or it can't be written.
    Q_INVOKABLE qint64 write(QQuickTextDocument *chapter, int start, int end, const QString &words);
    // Other words for a note; with none it is removed. Whether there is one of that number.
    Q_INVOKABLE bool change(qint64 id, const QString &words);
    Q_INVOKABLE void remove(qint64 id);
    // A lost note placed on that passage, where the reader selected it.
    Q_INVOKABLE bool placeAgain(qint64 id, QQuickTextDocument *chapter, int start, int end);
    // Where notes stand in the margin column (PageLayout::stack), each given
    // as {top, height}: their tops, in the order given. A `bottom` under 0 is none.
    Q_INVOKABLE QVariantList stack(const QVariantList &wanted, double spacing, double bottom) const;
    // Reads the open book's notes again: another computer's came, or the
    // file changed and they were found again.
    Q_INVOKABLE void refresh();

    // The same, for a text given as it is shown (the document's plain text,
    // soft hyphens and all): for the tests, and what the above call.
    [[nodiscard]] QList<std::pair<qint64, std::pair<qsizetype, qsizetype>>> spansIn(const QString &shown) const;
    [[nodiscard]] std::optional<qint64> write(const QString &shown, qsizetype start, qsizetype end, const QString &words);
    [[nodiscard]] bool placeAgain(qint64 id, const QString &shown, qsizetype start, qsizetype end);

signals:
    void changed();

private:
    [[nodiscard]] Library *library() const;
    [[nodiscard]] const Annotation *find(qint64 id) const;
    // The passage of the chapter shown from `start` to `end` of a text showing
    // it, placed in the book's own text; none when it holds no words.
    [[nodiscard]] std::optional<std::pair<Passage, double>> passageIn(const QString &shown, qsizetype start, qsizetype end) const;
    void read();
    void announce();

    QPointer<Reader> m_reader;
    QList<Annotation> m_notes, m_lost;
    QString m_fileHash; // of the file the open book is read from, which a new note is placed in
    bool m_drafting = false;
    int m_revision = 0;
    // The spans of the last text asked about, while neither it nor the notes changed.
    size_t m_spannedText = 0;
    int m_spannedRevision = -1;
    QVariantList m_spans;
};
