#pragma once

#include <QColor>
#include <QHash>
#include <QPointF>
#include <QQuickItem>
#include <QRectF>
#include <QSet>
#include <QTimer>
#include <QVariantMap>

#include <memory>
#include <utility>
#include <vector>

#include "inkinput.h"
#include "inklasso.h"
#include "inkselection.h"
#include "inksheet.h"
#include "inkwriting.h"

class InkNodes;
class QPointingDevice;
class QTabletEvent;

// A surface to write and draw on with a pen (#78), for Omabook and its
// plugins (`import Omabook`, `InkSurface {}`): the pen's pressure makes the
// line wider, its other end (or the eraser tool) erases parts of strokes or
// whole ones, and a palm on the screen is ignored while the pen is near
// (Ink::InputRules). A finger draws until a pen has been seen, then scrolls
// what is under the surface; a mouse draws too. Where fingers draw, two of
// them (#134) don't: what they began goes, and they move the Flickable the
// surface is on and say how far they spread (`pinched`), for what zooms.
// The lasso (#129) selects the strokes a path goes around, to drag,
// recolour, take away or copy (`selection`, InkSelection). Undo and redo go
// a step at a time: a stroke, an eraser's sweep, or what was done to a selection.
//
// The strokes are data (Ink::Stroke), not a picture: the surface draws them
// with the scene graph, and says what changed once the pen pauses, as an
// ink batch (`inked`), for whoever keeps them; `load` and `apply` take them
// back, from there or from another computer. Their points are in ink units:
// `inkScale` item pixels to one, so that book ink can follow a page's zoom.
class InkSurface : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY lookChanged)
    // In ink units, at a middling pressure.
    Q_PROPERTY(double penWidth READ penWidth WRITE setPenWidth NOTIFY lookChanged)
    // Whether the pen's pressure makes the line wider (#130); without it a
    // line is `penWidth` all along, whatever the pen says.
    Q_PROPERTY(bool pressure READ pressure WRITE setPressure NOTIFY lookChanged)
    // Draws as a highlighter (#130): the line laid on once, `color`'s alpha
    // and all, so it is no darker where it crosses itself. Kept with each
    // stroke, so it looks the same wherever the stroke is shown.
    Q_PROPERTY(bool highlighter READ highlighter WRITE setHighlighter NOTIFY lookChanged)
    // In item pixels, whatever the scale.
    Q_PROPERTY(double eraserWidth READ eraserWidth WRITE setEraserWidth NOTIFY lookChanged)
    Q_PROPERTY(double inkScale READ inkScale WRITE setInkScale NOTIFY lookChanged)
    Q_PROPERTY(Tool tool READ tool WRITE setTool NOTIFY toolChanged)
    Q_PROPERTY(EraserKind eraser READ eraser WRITE setEraser NOTIFY toolChanged)
    Q_PROPERTY(FingerUse fingers READ fingers WRITE setFingers NOTIFY toolChanged)
    Q_PROPERTY(bool mouseDraws READ mouseDraws WRITE setMouseDraws NOTIFY toolChanged)
    // Shows the strokes and takes no input.
    Q_PROPERTY(bool readOnly READ readOnly WRITE setReadOnly NOTIFY toolChanged)
    // How long the pen rests before what it did goes out as a batch, in milliseconds.
    Q_PROPERTY(int batchPause READ batchPause WRITE setBatchPause NOTIFY toolChanged)
    Q_PROPERTY(int count READ count NOTIFY strokesChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY strokesChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY strokesChanged)
    Q_PROPERTY(bool penSeen READ penSeen NOTIFY penChanged)
    Q_PROPERTY(bool penNear READ penNear NOTIFY penChanged)
    Q_PROPERTY(bool writing READ writing NOTIFY writingChanged)
    Q_PROPERTY(InkSelection *selection READ selection CONSTANT)

public:
    enum class Tool { Pen, Eraser, Lasso };
    Q_ENUM(Tool)
    enum class EraserKind { Parts, Strokes };
    Q_ENUM(EraserKind)
    enum class FingerUse { Auto, Draw, Scroll };
    Q_ENUM(FingerUse)

    explicit InkSurface(QQuickItem *parent = nullptr);
    ~InkSurface() override;
    InkSurface(const InkSurface &) = delete;
    InkSurface &operator=(const InkSurface &) = delete;
    InkSurface(InkSurface &&) = delete;
    InkSurface &operator=(InkSurface &&) = delete;

    [[nodiscard]] QColor color() const { return m_color; }
    void setColor(const QColor &color);
    [[nodiscard]] double penWidth() const { return m_penWidth; }
    void setPenWidth(double width);
    [[nodiscard]] bool pressure() const { return m_pressure; }
    void setPressure(bool pressure);
    [[nodiscard]] bool highlighter() const { return m_highlighter; }
    void setHighlighter(bool highlighter);
    [[nodiscard]] double eraserWidth() const { return m_eraserWidth; }
    void setEraserWidth(double width);
    [[nodiscard]] double inkScale() const { return m_inkScale; }
    void setInkScale(double scale);
    [[nodiscard]] Tool tool() const { return m_tool; }
    void setTool(Tool tool);
    [[nodiscard]] EraserKind eraser() const { return m_eraser; }
    void setEraser(EraserKind kind);
    [[nodiscard]] FingerUse fingers() const { return m_fingers; }
    void setFingers(FingerUse use);
    [[nodiscard]] bool mouseDraws() const { return m_mouseDraws; }
    void setMouseDraws(bool draws);
    [[nodiscard]] bool readOnly() const { return m_readOnly; }
    void setReadOnly(bool readOnly);
    [[nodiscard]] int batchPause() const { return m_batchTimer.interval(); }
    void setBatchPause(int milliseconds);
    [[nodiscard]] int count() const { return static_cast<int>(m_sheet.strokes().size()); }
    [[nodiscard]] bool canUndo() const { return m_sheet.canUndo(); }
    [[nodiscard]] bool canRedo() const { return m_sheet.canRedo(); }
    [[nodiscard]] bool penSeen() const { return m_rules.penSeen(); }
    [[nodiscard]] bool penNear() const { return m_rules.penNear(); }
    [[nodiscard]] bool writing() const { return !m_writing.empty(); }
    [[nodiscard]] InkSelection *selection() { return &m_selection; }
    // Writing, or two fingers moving what is under the surface; or a touch
    // of the event of this moment (its timestamp) was the surface's.
    [[nodiscard]] bool touched(quint64 at = 0) const { return writing() || !m_gesture.isEmpty() || (at != 0 && at == m_touchedAt); }

    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    // Erases every stroke, as one step to undo.
    Q_INVOKABLE void clear();
    // Sends what is waiting for the pen to pause now.
    Q_INVOKABLE void flush();
    // Every stroke, {id: base64 of its bytes}: the items of a set in the change log.
    Q_INVOKABLE QVariantMap strokes() const;
    // Shows these strokes in place of the ones there, with nothing to undo;
    // a stroke that can't be read is left out. Answers how many were read.
    Q_INVOKABLE int load(const QVariantMap &strokes);
    // Takes a batch from where the strokes are kept or another computer
    // (`{add, remove}`, as `inked` sends), not as an edit. False, and nothing
    // taken, when a stroke of it can't be read.
    Q_INVOKABLE bool apply(const QVariantMap &batch);
    // Where each stroke is, {id: rect}, and where one is (an empty rect for
    // none), in item pixels with its line's width, as `selection.box` is (#133).
    Q_INVOKABLE QVariantMap bounds() const;
    Q_INVOKABLE QRectF boundsOf(const QString &id) const;
    // Every stroke as one base64 text (Ink::encodeAll), and back.
    Q_INVOKABLE QString snapshot() const;
    Q_INVOKABLE bool restore(const QString &snapshot);
    // What the last frame's strokes took to put together, in milliseconds of its thread's work (ThreadClock).
    Q_INVOKABLE double frameTime() const { return m_frameTime; }

    // The same for C++ (book ink, #83).
    [[nodiscard]] const Ink::Sheet &sheet() const { return m_sheet; }
    void replace(std::vector<Ink::Stroke> strokes);
    void applyChange(const Ink::Change &change);

    // What tells a palm from a finger. It starts knowing a pen when one was
    // used in this run, on another surface; a pen Qt merely lists is none
    // (#134). A test starts it afresh.
    [[nodiscard]] Ink::InputRules &rules() { return m_rules; }

signals:
    void lookChanged();
    void toolChanged();
    void strokesChanged();
    void penChanged();
    void writingChanged();
    // What was done since the last batch, `{"add": {id: base64}, "remove": [id]}`,
    // once the pen pauses: one `omabook.log.add(entity, id, set, batch.add, batch.remove)`.
    void inked(const QVariantMap &batch);
    // Two fingers spread apart (more than 1) or came together, by this much since the last time.
    void pinched(double scale);

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *data) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void touchEvent(QTouchEvent *event) override;
    void touchUngrabEvent() override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    bool tablet(QTabletEvent &event);
    void touchPoint(const QTouchEvent &event, QEventPoint &point);
    // What a touch just down does, by the rules and the tool down.
    [[nodiscard]] Ink::Act actOf(const QTouchEvent &event, const QEventPoint &point) const;
    // Two fingers that would write, one just down: a gesture, not writing.
    [[nodiscard]] bool twoFingers(const QTouchEvent &event) const;
    void beginGesture(const QTouchEvent &event);
    void gesture(QTouchEvent &event);
    // The fingers moved from the first of each pair to the second, in the scene.
    void follow(const std::vector<std::pair<QPointF, QPointF>> &moved);
    void moveUnder(QPointF by);
    // A pointer down: it writes, or erases with the eraser chosen (or its own end).
    void begin(Ink::Pointer pointer, const Ink::Input &input, bool erasing, bool touch, const QPointingDevice *device = nullptr);
    void lift(Ink::Pointer pointer, const Ink::Input &input);
    void drop(Ink::Pointer pointer);
    void dropTouches();
    // An event's place, in ink units.
    [[nodiscard]] Ink::Input inputAt(QPointF inItem, quint64 ms) const;
    void edited(Ink::Change change);
    [[nodiscard]] double millimetresPerPixel() const;

    Ink::Sheet m_sheet;
    Ink::Lasso m_lasso{m_sheet};
    Ink::Writing m_writing{m_sheet, &m_lasso};
    InkSelection m_selection;
    Ink::InputRules m_rules;
    QSet<Ink::Pointer> m_palms;
    // The fingers of a gesture, where each was last, in the scene.
    QHash<Ink::Pointer, QPointF> m_gesture;
    // When the last touch the surface took came.
    quint64 m_touchedAt = 0;
    Ink::Change m_pending;
    QTimer m_batchTimer;

    QColor m_color{Qt::black};
    double m_penWidth = 2.0;
    bool m_pressure = true;
    bool m_highlighter = false;
    double m_eraserWidth = 24.0;
    double m_inkScale = 1.0;
    Tool m_tool = Tool::Pen;
    EraserKind m_eraser = EraserKind::Parts;
    FingerUse m_fingers = FingerUse::Auto;
    bool m_mouseDraws = true;
    bool m_readOnly = false;

    // What the next frame redraws: the strokes whose lines changed, and
    // whether any came, went or moved one over another.
    QSet<QString> m_dirty;
    bool m_restructured = true;
    bool m_allDirty = true;
    std::unique_ptr<InkNodes> m_nodes;
    double m_frameTime = 0.0;
};

// Makes the ink surface known to QML, as `InkSurface` of `Omabook 1.0`.
void registerInkTypes();
