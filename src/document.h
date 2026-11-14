#pragma once

#include <QImage>
#include <QList>
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
};
