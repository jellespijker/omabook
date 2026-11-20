#pragma once

#include <QColor>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QVariantList>
#include <QVariantMap>
#include <utility>
#include <vector>

#include "inkstroke.h"

class InkSurface;
class Reader;

// The open book's ink (#83), as the reading view draws and shows it: one ink
// surface over each page of a PDF or a comic as it is drawn, its strokes the
// page's (BookInk) at the page's zoom, and what the pen does there kept in the
// change log at each pause of the pen, an ink batch a write. Undo and redo go
// a batch at a time over every page, the surfaces' own steps or not, and
// survive a page scrolled out of sight. What another computer drew comes in
// when the library is read again after a sync, onto the pages shown.
//
// While writing (`inking`) the pen, and the mouse, write and erase on the
// pages (the pen's other end erases, as the eraser tool does), and the pen's
// gestures that highlight rest; fingers still scroll and zoom. `shown` puts
// the ink away, for reading the pages as they were.
class ReadingInk : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool inking READ inking WRITE setInking NOTIFY modeChanged)
    Q_PROPERTY(bool shown READ shown WRITE setShown NOTIFY modeChanged)
    Q_PROPERTY(bool erasing READ erasing WRITE setErasing NOTIFY modeChanged)
    // What the pen writes with: one of `colors`, by its key.
    Q_PROPERTY(QString color READ color WRITE setColor NOTIFY modeChanged)
    Q_PROPERTY(QColor penColor READ penColor NOTIFY modeChanged)
    // The inks there are: [{key, color}], dark enough for a white page.
    Q_PROPERTY(QVariantList colors READ colors CONSTANT)
    // Ink units across a page (BookInk::pageUnits): a page's surface draws at its width over this.
    Q_PROPERTY(double pageUnits READ pageUnits CONSTANT)
    // The open book's pages with ink, in its order: [{id, page, strokes, x, y,
    // width, height, fraction}], the part each covers measured in the page's
    // width (PageRegion), and where that starts down the page.
    Q_PROPERTY(QVariantList pages READ pages NOTIFY changed)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)

public:
    explicit ReadingInk(Reader *reader);

    [[nodiscard]] bool inking() const noexcept { return m_inking; }
    void setInking(bool inking);
    [[nodiscard]] bool shown() const noexcept { return m_shown; }
    void setShown(bool shown);
    [[nodiscard]] bool erasing() const noexcept { return m_erasing; }
    void setErasing(bool erasing);
    [[nodiscard]] QString color() const { return m_color; }
    void setColor(const QString &color);
    [[nodiscard]] QColor penColor() const;
    [[nodiscard]] static QVariantList colors();
    [[nodiscard]] static double pageUnits();
    [[nodiscard]] QVariantList pages() const;
    [[nodiscard]] bool canUndo() const noexcept { return !m_undo.empty(); }
    [[nodiscard]] bool canRedo() const noexcept { return !m_redo.empty(); }

    // An ink surface over a page (or a picture of it): it shows the page's
    // strokes, and those that come, until it is forgotten or goes.
    Q_INVOKABLE void show(QQuickItem *surface, int page);
    Q_INVOKABLE void forget(QQuickItem *surface);
    // An ink batch a page's surface sent (`inked`): kept, and a step of undo.
    Q_INVOKABLE bool take(QQuickItem *surface, const QVariantMap &batch);
    Q_INVOKABLE bool undo();
    Q_INVOKABLE bool redo();
    // All the ink on a page taken away, as a step of undo: the list's.
    Q_INVOKABLE bool erasePage(int page);
    // Reads the ink again: another computer's came.
    Q_INVOKABLE void refresh();

    // The same for C++: a change on a page, and the strokes on one.
    bool take(int page, const Ink::Change &change);
    [[nodiscard]] std::vector<Ink::Stroke> strokesOn(int page);

signals:
    void changed();
    void modeChanged();

private:
    // A step of undo: what a batch put on a page and took off it, whole.
    struct Step {
        int page = 0;
        std::vector<Ink::Stroke> added;
        std::vector<Ink::Stroke> removed;
    };
    [[nodiscard]] static Ink::Change changeOf(const Step &step);
    [[nodiscard]] static Step inverse(Step step);
    // A change of a page kept, as a step of undo.
    bool record(int page, const Ink::Change &change, const InkSurface *from);
    // Keeps a change of a page and shows it on its surfaces but `from`, which drew it.
    bool keep(int page, const Ink::Change &change, const InkSurface *from);
    void flushAll();
    void readList();
    void clear();
    [[nodiscard]] QString fileHash();

    Reader *m_reader;
    bool m_inking = false;
    bool m_shown = true;
    bool m_erasing = false;
    QString m_color = QStringLiteral("black");
    // The strokes of the pages asked about, as the log has them.
    QHash<int, std::vector<Ink::Stroke>> m_pages;
    QList<std::pair<int, QPointer<InkSurface>>> m_surfaces;
    std::vector<Step> m_undo, m_redo;
    QVariantList m_list;
    // The file the open book is read from, which new ink is placed in.
    QString m_fileHash;
    qint64 m_hashOf = 0;
};
