#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QSet>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QVariantList>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "inkstroke.h"
#include "document.h"
#include "library.h"

class NoteRecognition;
class QQuickItem;
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
//
// A note written by hand (#90) is its ink: strokes kept on the annotation in
// the change log (AnnotationInk), in ink units `inkUnits` across its card,
// whatever the card's width; its words are what was read of the ink
// (NoteRecognition), searched and shown when the reader asks.
class MarginNotes : public QObject {
    Q_OBJECT
    // The reader whose open book's notes these are (the window's `reader`).
    Q_PROPERTY(QObject *reader READ reader WRITE setReader NOTIFY changed)
    // The settings handwriting is read by (the window's `settings`); none, and it isn't read.
    Q_PROPERTY(QObject *settings READ settings WRITE setSettings NOTIFY changed)
    Q_PROPERTY(double inkUnits READ inkUnits CONSTANT)
    // Changes whenever how the reading of a note's handwriting goes changes (readingOf).
    Q_PROPERTY(int readings READ readings NOTIFY readingsChanged)
    // Its notes on a passage that is there, in the order of the book: [{id,
    // section, fraction, quote, note, ink}]; and those that are lost, the same.
    Q_PROPERTY(QVariantList notes READ notes NOTIFY changed)
    Q_PROPERTY(QVariantList lost READ lost NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)
    // A note being written on a passage that has none yet, which the page
    // makes room for as for one that is there.
    Q_PROPERTY(bool drafting READ drafting WRITE setDrafting NOTIFY changed)
    // Others' notes are shown on the book too (AnnotationLayerView, #85), which want the margin column as these do.
    Q_PROPERTY(bool othersWanted READ othersWanted WRITE setOthersWanted NOTIFY changed)
    // Changes with every write and every new chapter, for what is worked out from them.
    Q_PROPERTY(int revision READ revision NOTIFY changed)

public:
    // How wide a handwritten note's ink is, in ink units.
    static constexpr double inkWidth = 240;

    explicit MarginNotes(QObject *parent = nullptr);
    ~MarginNotes() override;
    Q_DISABLE_COPY_MOVE(MarginNotes)

    QObject *reader() const;
    void setReader(QObject *reader);
    QObject *settings() const { return m_settings; }
    void setSettings(QObject *settings);
    double inkUnits() const { return inkWidth; }
    int readings() const noexcept { return m_readings; }
    QVariantList notes() const;
    QVariantList lost() const;
    int count() const { return static_cast<int>(m_notes.size()); }
    bool drafting() const noexcept { return m_drafting; }
    void setDrafting(bool drafting);
    int revision() const noexcept { return m_revision; }
    bool othersWanted() const noexcept { return m_othersWanted; }
    void setOthersWanted(bool wanted);

    // Where the notes of the chapter shown are in a text showing it (a
    // TextEdit's document): [{id, start, end, note, ink}], in the order of their
    // passages; a note whose words it doesn't hold is left out.
    Q_INVOKABLE QVariantList spans(QQuickTextDocument *chapter);
    // A note with these words on the passage of the chapter shown from
    // character `start` to `end` of the document: its number, or 0 when
    // there are no words, no words there, or it can't be written.
    Q_INVOKABLE qint64 write(QQuickTextDocument *chapter, int start, int end, const QString &words);
    // Other words for a note; with none it is removed. Whether there is one of that number.
    Q_INVOKABLE bool change(qint64 id, const QString &words);
    // Whether there was one to remove, in a book that is still there (#229).
    Q_INVOKABLE bool remove(qint64 id);
    // A lost note placed on that passage, where the reader selected it.
    Q_INVOKABLE bool placeAgain(qint64 id, QQuickTextDocument *chapter, int start, int end);
    // On the pages of a PDF or a comic (#115), which stand still: the notes of
    // a page shown `shown` pixels, where they are on it: [{id, note, part,
    // x, y, width, height, endX, endTop, endY}], the rectangle around the
    // words of the passage and where its last word ends (endX, from the top
    // to the foot of its line), or the part of the page (`part`) a note on a
    // panel is on.
    Q_INVOKABLE QVariantList pageNotes(int page, QSizeF shown);
    // A note with these words on the words of a PDF's page from character
    // `start` to `end` of the page's text, widened to whole words; or on a
    // part of a page `aspect` tall (in the page's width, PageRegion), such
    // as a comic's panel. Its number, or 0.
    Q_INVOKABLE qint64 writeOnPage(int page, int start, int end, const QString &words);
    Q_INVOKABLE qint64 writeOnPart(int page, const QRectF &area, double aspect, const QString &words);
    // A lost note placed on words of a PDF's page.
    Q_INVOKABLE bool placeAgainOnPage(qint64 id, int page, int start, int end);
    // Where notes stand in the margin column (PageLayout::stack), each given
    // as {top, height}: their tops, in the order given. A `bottom` under 0 is none.
    Q_INVOKABLE QVariantList stack(const QVariantList &wanted, double spacing, double bottom) const;
    // Reads the open book's notes again: another computer's came, or the
    // file changed and they were found again.
    Q_INVOKABLE void refresh();

    // A note written by hand on the passage from `start` to `end`: its first
    // strokes those of `snapshot` (InkSurface::snapshot) that `which` counts,
    // `scale` ink units to a pixel of theirs, moved to the note's top corner.
    // Its number, or 0 when there are no words there or no strokes.
    Q_INVOKABLE qint64 writeInk(QQuickTextDocument *chapter, int start, int end, const QString &snapshot, const QVariantList &which,
                                double scale);
    // A note's strokes, {id: base64}, for an ink surface to load; and an ink
    // batch of its surface's (`inked`), kept, the note read again once the
    // pen rests. A note whose last stroke is erased is removed.
    Q_INVOKABLE QVariantMap strokes(qint64 id) const;
    Q_INVOKABLE bool ink(qint64 id, const QVariantMap &batch);
    // Reads a note's handwriting now, as the reader asked; and how that goes:
    // being read, or why it isn't, empty when neither.
    Q_INVOKABLE void recognize(qint64 id);
    Q_INVOKABLE QString readingOf(qint64 id) const;
    // The lowest of an ink surface's strokes, in ink units; 0 with none.
    Q_INVOKABLE double inkBottom(QQuickItem *surface) const;
    // The notes on a passage that is there whose words have these in them, for
    // the find in the book: [{section, fraction, snippet, note}], `note` its number.
    Q_INVOKABLE QVariantList found(const QString &words) const;

    // The same, for a text given as it is shown (the document's plain text,
    // soft hyphens and all): for the tests, and what the above call.
    [[nodiscard]] QList<std::pair<qint64, std::pair<qsizetype, qsizetype>>> spansIn(const QString &shown) const;
    [[nodiscard]] std::optional<qint64> write(const QString &shown, qsizetype start, qsizetype end, const QString &words);
    [[nodiscard]] bool placeAgain(qint64 id, const QString &shown, qsizetype start, qsizetype end);
    // The strokes in ink units of the note's, from pixels `scale` units each.
    [[nodiscard]] std::optional<qint64> writeInk(const QString &shown, qsizetype start, qsizetype end, std::vector<Ink::Stroke> strokes,
                                                 double scale);
    bool ink(qint64 id, const Ink::Change &change);
    [[nodiscard]] bool inked(qint64 id) const { return m_inked.contains(id); }
    [[nodiscard]] NoteRecognition *recognition() const { return m_recognition.get(); }

signals:
    void changed();
    void readingsChanged();

private:
    [[nodiscard]] Library *library() const;
    [[nodiscard]] const Annotation *find(qint64 id) const;
    // The passage of the chapter shown from `start` to `end` of a text showing
    // it, placed in the book's own text; none when it holds no words.
    [[nodiscard]] std::optional<std::pair<Passage, double>> passageIn(const QString &shown, qsizetype start, qsizetype end) const;
    // A note on that passage, placed in the file the book is read from.
    [[nodiscard]] Annotation noteOn(const std::pair<Passage, double> &passage, const QString &words) const;
    // The passage of a page of the book from character `start` to `end` of its text, widened to whole words.
    [[nodiscard]] std::optional<Passage> passageOnPage(int page, int start, int end) const;
    // A new note kept, its number; 0 when it can't be.
    [[nodiscard]] qint64 add(const Annotation &note);
    void read();
    void announce();

    QPointer<Reader> m_reader;
    QPointer<QObject> m_settings;
    std::unique_ptr<NoteRecognition> m_recognition;
    int m_readings = 0;
    QList<Annotation> m_notes, m_lost;
    QSet<qint64> m_inked; // the notes written by hand
    QString m_fileHash; // of the file the open book is read from, which a new note is placed in
    bool m_drafting = false;
    bool m_othersWanted = false;
    int m_revision = 0;
    // The spans of the last text asked about, while neither it nor the notes changed.
    size_t m_spannedText = 0;
    int m_spannedRevision = -1;
    QVariantList m_spans;
    // The words of the pages of a PDF asked about, as Poppler found them.
    QHash<int, QList<PageWord>> m_words;
};
