#pragma once

#include <QColor>
#include <QHash>
#include <QObject>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QSizeF>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

#include "document.h"
#include "library.h"
#include "textgeometry.h"

class Reader;

// The open book's highlights (#81), as the reader shows and makes them: on
// the chapter a TextEdit shows, or on a page of a PDF as it is drawn. A
// highlight is made from words selected (by the mouse, the keys or a long
// press) or by a stroke of the pen over the text (Ink::gestureOf: under,
// around or beside words; a scribble takes highlights away), in one of the
// named colours (HighlightColors), coloured anew, removed, and listed, the
// lost ones too, which are placed again on words selected.
//
// Where a highlight stands on the page is worked out from its words each
// time it is asked (Passage::spanIn), so a change of type, width or edition
// never leaves one beside its words.
class ReadingHighlights : public QObject {
    Q_OBJECT
    // The theme, for how each colour is laid over the page.
    Q_PROPERTY(QColor foreground READ foreground WRITE setForeground NOTIFY lookChanged)
    Q_PROPERTY(QColor background READ background WRITE setBackground NOTIFY lookChanged)
    // The five as the page draws them: [{key, color, tint, line, paperTint, paperLine, dashes}].
    Q_PROPERTY(QVariantList colors READ colors NOTIFY lookChanged)
    // The colour new ones are made in: the last one chosen.
    Q_PROPERTY(QString current READ current WRITE setCurrent NOTIFY currentChanged)
    // The open book's, in its order: [{id, color, quote, section, fraction, title, page, lost}], and
    // one on a part of a page its `region` too ({page, x, y, width, height}, PageRegion).
    Q_PROPERTY(QVariantList list READ list NOTIFY changed)
    Q_PROPERTY(int lostCount READ lostCount NOTIFY changed)

public:
    explicit ReadingHighlights(Reader *reader);

    [[nodiscard]] QColor foreground() const noexcept { return m_foreground; }
    void setForeground(const QColor &color);
    [[nodiscard]] QColor background() const noexcept { return m_background; }
    void setBackground(const QColor &color);
    [[nodiscard]] QVariantList colors() const;
    [[nodiscard]] QString current() const { return m_current; }
    void setCurrent(const QString &color);
    [[nodiscard]] QVariantList list() const;
    [[nodiscard]] int lostCount() const;

    // On a chapter a TextEdit shows, by its characters.
    // Each highlight of it, where it stands: [{id, color, rects: [rect]}].
    Q_INVOKABLE QVariantList spansIn(QQuickTextDocument *chapter) const;
    // The highlight under a stretch of it (the first, when there are more): {id, color}; empty when none.
    Q_INVOKABLE QVariantMap at(QQuickTextDocument *chapter, int start, int end) const;
    // A new one on that stretch, widened to whole words; its number, 0 when there are no words.
    Q_INVOKABLE qint64 highlight(QQuickTextDocument *chapter, int start, int end, const QString &color);
    // A lost one placed on that stretch.
    Q_INVOKABLE bool placeAgain(qint64 id, QQuickTextDocument *chapter, int start, int end);
    // A stroke of the pen over it, its points in the TextEdit's coordinates,
    // done: {kind: "underline", "circle", "bracket" or "scribble", id (made),
    // removed (how many)}; kind "" when it means nothing here.
    Q_INVOKABLE QVariantMap penned(QQuickTextDocument *chapter, const QVariantList &points);

    // The same on a page of a PDF shown at `shown` pixels, its characters those of the page's text.
    Q_INVOKABLE QVariantList pageSpans(int page, QSizeF shown) const;
    Q_INVOKABLE QVariantMap pageAt(int page, int start, int end) const;
    Q_INVOKABLE qint64 highlightPage(int page, int start, int end, const QString &color);
    Q_INVOKABLE bool placeAgainOnPage(qint64 id, int page, int start, int end);
    Q_INVOKABLE QVariantMap pagePenned(int page, QSizeF shown, const QVariantList &points);
    // The page's words where they stand, for selecting them: [{x, y, width, height, start, end}].
    Q_INVOKABLE QVariantList pageWords(int page, QSizeF shown) const;
    // The words of a page from one character to another.
    Q_INVOKABLE QString pageText(int page, int start, int end) const;

    // On a part of a page, a comic's panel, which has no words (#115): those
    // of a page shown `width` pixels wide, where they stand: [{id, color, x,
    // y, width, height}]; and a new one on a part (in the page's width,
    // PageRegion) of a page `aspect` tall, its number, 0 when it can't be made.
    Q_INVOKABLE QVariantList pageParts(int page, double width) const;
    Q_INVOKABLE qint64 highlightPart(int page, const QRectF &area, double aspect, const QString &color);

    // The strokes an ink surface over the page holds, each its points in the
    // surface's coordinates ({x, y, ms}, ms since the stroke began, which
    // tells the pen held still from a tap, #140), taken off it: the pen's
    // gestures are acted on, not kept.
    Q_INVOKABLE QVariantList takeStrokes(QQuickItem *surface);

    Q_INVOKABLE bool recolor(qint64 id, const QString &color);
    Q_INVOKABLE void remove(qint64 id);
    // The open book's highlights as Markdown, of one colour or ('') all, each
    // colour called as `names` says ({key: name}).
    Q_INVOKABLE QString exported(const QString &color, const QVariantMap &names) const;
    // Reads them again from the library: synced, found again, changed elsewhere.
    Q_INVOKABLE void refresh();

signals:
    void changed();
    void lookChanged();
    void currentChanged();

private:
    // What the reader shows of a section, as a highlight needs it.
    struct Shown {
        int section = 0;
        QString text;
        std::function<QList<TextGeometry::Line>(const QRectF &)> linesIn;
        std::function<QList<QRectF>(int, int)> rects;
        std::function<int(const TextGeometry::Line &, double, bool)> positionOn;
    };
    [[nodiscard]] Shown chapterShown(QQuickTextDocument *chapter) const;
    [[nodiscard]] Shown pageShown(int page, QSizeF shown) const;
    [[nodiscard]] QVariantList spans(const Shown &shown) const;
    [[nodiscard]] QVariantMap under(const Shown &shown, int start, int end) const;
    qint64 make(const Shown &shown, int start, int end, const QString &color);
    bool place(qint64 id, const Shown &shown, int start, int end);
    QVariantMap pen(const Shown &shown, const QVariantList &points);
    // Where each highlight of the section shown is in its text: {id, start, end}.
    struct Span {
        qint64 id = 0;
        qsizetype start = 0, end = 0;
    };
    [[nodiscard]] QList<Span> placed(const Shown &shown) const;
    [[nodiscard]] QString titleOf(int section) const;

    Reader *m_reader;
    QList<Annotation> m_highlights;
    // The words of the pages of a PDF asked about, as Poppler found them.
    mutable QHash<int, QList<PageWord>> m_words;
    QColor m_foreground{Qt::white}, m_background{Qt::black};
    QString m_current = QStringLiteral("yellow");
};
