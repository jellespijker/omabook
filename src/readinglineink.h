#pragma once

#include <QColor>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <optional>
#include <vector>

#include "inkstroke.h"
#include "lineink.h"

class AnnotationLayerView;
class InkSurface;
class QTextDocument;
class Reader;
class ReadingInk;

// The open flowing book's free ink (#152, ADR 0010), as the reading view draws
// it and the pen writes it: one ink surface over each laid-out chapter shown
// (a page that scrolls, or each page of a spread), reaching past the text into
// the page's side margins. What the pen does there is kept at each pause of
// the pen, each stroke on the line beside it (LineInk), and drawn again where
// that line is whenever the chapter is laid out anew: another size or face of
// type, another width, scrolling or pages. Undo and redo go a batch at a time.
//
// The pen's mode (writing or not, the tool, the ink, whether a finger writes,
// #134) is the reader's, as on a PDF's pages (ReadingInk). The inks are drawn to
// read on the page as the theme colours it: the first is the text's own colour,
// the others lighter on a dark page, kept as the inks they are. Others' free
// ink in a space (#85) is drawn beside the same lines, in their colours.
class ReadingLineInk : public QObject {
    Q_OBJECT
    Q_PROPERTY(QColor foreground READ foreground WRITE setForeground NOTIFY lookChanged)
    Q_PROPERTY(QColor background READ background WRITE setBackground NOTIFY lookChanged)
    // The pen's ink as it is drawn on this page (ReadingInk::penColor, shown).
    Q_PROPERTY(QColor penColor READ penColor NOTIFY lookChanged)
    // The chapters with free ink, in the order of the book: [{id, section,
    // fraction, title, quote, lines, strokes}], `quote` the words of its first line.
    Q_PROPERTY(QVariantList list READ list NOTIFY changed)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)

public:
    ReadingLineInk(Reader *reader, ReadingInk *mode);

    [[nodiscard]] QColor foreground() const noexcept { return m_foreground; }
    void setForeground(const QColor &color);
    [[nodiscard]] QColor background() const noexcept { return m_background; }
    void setBackground(const QColor &color);
    [[nodiscard]] QColor penColor() const;
    [[nodiscard]] QVariantList list() const { return m_list; }
    [[nodiscard]] bool canUndo() const noexcept { return !m_undo.empty(); }
    [[nodiscard]] bool canRedo() const noexcept { return !m_redo.empty(); }

    // An ink surface over the chapter a TextEdit shows: it shows the
    // chapter's free ink, drawn again as the text is laid out again, until it
    // is forgotten or goes.
    Q_INVOKABLE void show(QQuickItem *surface, QQuickItem *text);
    // A read-only surface over it that shows others' free ink there instead (#85, AnnotationLayerView `others`).
    Q_INVOKABLE void showTheirs(QQuickItem *surface, QQuickItem *text, QObject *others);
    Q_INVOKABLE void forget(QQuickItem *surface);
    // An ink batch a surface sent (`inked`): kept, and a step of undo.
    Q_INVOKABLE bool take(QQuickItem *surface, const QVariantMap &batch);
    Q_INVOKABLE bool undo();
    Q_INVOKABLE bool redo();
    // All the free ink of a chapter taken away, as a step of undo: the list's.
    Q_INVOKABLE bool eraseSection(int section);
    // Reads the ink again: another computer's came.
    Q_INVOKABLE void refresh();
    // Whether a finger is on the ink, or the touch of this moment was its (ReadingInk::touched).
    Q_INVOKABLE bool touched(qulonglong at = 0) const;
    // An ink (ReadingInk::colors) as it is drawn on this page.
    Q_INVOKABLE QColor shownColor(QColor kept) const;
    // Each surface drawn again now, as the text is laid out (what a change of it does, a moment later).
    Q_INVOKABLE void relay();

    // The same for C++ (the tests): a change on a surface, in its pixels.
    bool take(const InkSurface &surface, const Ink::Change &change);
    [[nodiscard]] const QList<LineInk::Line> &lines() const noexcept { return m_lines; }

signals:
    void changed();
    void lookChanged();

private:
    struct Shown {
        QPointer<InkSurface> surface;
        QPointer<QQuickItem> text;
        QPointer<AnnotationLayerView> others;
    };
    // A chapter as a surface shows it: its text laid out, the column it is
    // set in, where the surface stands on it, and its lines of ink placed there.
    struct View {
        const QTextDocument *document = nullptr;
        QString shown;
        InkLines::Column column;
        QPointF offset;
        QList<LineInk::Line> lines;
        QList<LineInk::Placed> placed;
    };
    // What a step did to one line, in its own units; a line made again after its last stroke was undone.
    struct Part {
        qint64 id = 0;
        QString kind;
        FoundPassage passage;
        std::vector<Ink::Stroke> added;
        std::vector<Ink::Stroke> removed;
    };
    using Step = std::vector<Part>;

    [[nodiscard]] std::optional<View> viewOf(const Shown &shown) const;
    [[nodiscard]] std::vector<Ink::Stroke> drawn(const View &view) const;
    // Keeps what a surface did (in its pixels) as changes of lines.
    [[nodiscard]] std::optional<Step> partsOf(const View &view, const Ink::Change &change) const;
    bool apply(Part &part, const Ink::Change &change);
    bool perform(Step &step, bool forward);
    // A surface drawn again: whole (laid out anew), or only where it differs.
    void redraw(const Shown &shown, bool whole);
    void redrawAll(bool whole);
    void reread();
    void flushAll();
    void clear();
    [[nodiscard]] QColor keptColor(QColor shown) const;
    [[nodiscard]] QString fileHash() const;

    Reader *m_reader;
    ReadingInk *m_mode;
    QList<Shown> m_shown;
    QList<LineInk::Line> m_lines;
    std::vector<Step> m_undo, m_redo;
    QVariantList m_list;
    QColor m_foreground{Qt::black}, m_background{Qt::white};
    // The text laid out again: each surface is drawn again once, after.
    QTimer m_relaid;
};
