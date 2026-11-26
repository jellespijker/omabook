#pragma once

#include <QImage>
#include <QList>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <memory>

#include "bookmeta.h"

struct TocEntry {
    QString title;
    int section = 0;
    QString anchor;
    int level = 0;
};

// A word of a page and where it stands on it (#81): its box as a share of the
// page's width and height (0 to 1), and where it is in the page's text
// (Document::text), its first character and the one past its last.
struct PageWord {
    QRectF box;
    qsizetype start = 0;
    qsizetype end = 0;
};

// An opened book, whatever its format. A book either flows (chapters of text
// that Omabook sets in the reader's own font and theme) or is made of fixed
// pages (a PDF, a comic) that are drawn as pictures. The reader, the command
// line and the AI all see books only through this, so a new format is one
// subclass and one line in formats.cpp.
//
// Implementations are safe to call from any thread: pages are rendered off
// the GUI thread while the reader asks for text on it.
class Document {
public:
    enum Kind { Flow, Pages };

    virtual ~Document() = default;

    virtual Kind kind() const = 0;
    // Chapters of a flowing book, pages of a paged one.
    virtual int count() const = 0;
    virtual BookMeta meta() const = 0;
    virtual QList<TocEntry> toc() const { return {}; }
    // Flow: which way the pages follow each other, when the book says so;
    // by default from its language, when that is written right to left.
    // Qt::LayoutDirectionAuto leaves it to whoever shows the book (#232).
    virtual Qt::LayoutDirection pageDirection() const;

    // Flow: one chapter as the rich text subset Qt lays out, with pictures no
    // wider than `width` pixels. Links to other chapters read
    // "omabook:<section>#<anchor>" and pictures "image://book/<name>".
    virtual QString html(int section, int width) const;
    // A picture a chapter refers to.
    virtual QByteArray resource(const QString &name) const;

    // Pages: one page drawn `width` pixels wide, and its size in points.
    virtual QImage image(int page, int width) const;
    virtual QSizeF pageSize(int page) const;

    // The words of a chapter or page, for search, agents and the AI.
    virtual QString text(int section) const;
    // Pages: the words of a page where they stand on it, in the order of its
    // text, for highlighting them; none for pages of pictures (a comic).
    virtual QList<PageWord> words(int page) const;
};
