#include "inkselection.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMimeData>
#include <QQuickItem>

#include <algorithm>
#include <memory>
#include <tuple>

#include "inkcodec.h"

namespace {

// Ink on the clipboard: Ink::encodeAll's bytes, the strokes with their ids.
constexpr auto inkType = "application/x-omabook-ink";
// How far a copy pasted beside what it is a copy of lies from it, in item pixels.
constexpr double besideBy = 12.0;
// The lasso's lines, in item pixels.
constexpr double lineWidth = 1.5;
constexpr double bigStep = 10.0;

const QMimeData *inkOnClipboard() {
    const QMimeData *data = QGuiApplication::clipboard()->mimeData();
    return data != nullptr && data->hasFormat(QString::fromLatin1(inkType)) ? data : nullptr;
}

// Which way an arrow key moves, a pixel; none for any other key.
QPointF arrowOf(int key) {
    switch (key) {
    case Qt::Key_Left:
        return {-1.0, 0.0};
    case Qt::Key_Right:
        return {1.0, 0.0};
    case Qt::Key_Up:
        return {0.0, -1.0};
    case Qt::Key_Down:
        return {0.0, 1.0};
    default:
        return {};
    }
}

} // namespace

InkSelection::InkSelection(Ink::Lasso &lasso, QQuickItem &surface, std::function<void(Ink::Change)> edit)
    : m_lasso(&lasso), m_surface(&surface), m_edit(std::move(edit)) {
    m_lasso->onChange([this] { told(); });
    surface.installEventFilter(this);
}

QRectF InkSelection::box() const {
    const QRectF box = m_lasso->box();
    return {box.topLeft() * m_scale, box.size() * m_scale};
}

void InkSelection::setColor(const QColor &color) {
    if (m_color != color) {
        m_color = color;
        m_surface->update();
        emit colorChanged();
    }
}

void InkSelection::setReadOnly(bool readOnly) {
    m_readOnly = readOnly;
    if (readOnly) {
        m_lasso->cancel();
    }
}

void InkSelection::selectAll() { m_lasso->selectAll(); }

void InkSelection::clear() { m_lasso->deselect(); }

QVariantMap InkSelection::strokes() const { return Ink::items(m_lasso->selected()).toVariantMap(); }

void InkSelection::moveBy(double dx, double dy) {
    if (!m_readOnly) {
        edit(m_lasso->move(QPointF(dx, dy) / m_scale));
    }
}

void InkSelection::recolor(const QColor &color) {
    if (!m_readOnly) {
        edit(m_lasso->recolor(color));
    }
}

void InkSelection::remove() {
    if (!m_readOnly) {
        edit(m_lasso->remove());
    }
}

void InkSelection::copy() {
    const std::vector<Ink::Stroke> strokes = m_lasso->selected();
    if (strokes.empty()) {
        return;
    }
    auto data = std::make_unique<QMimeData>();
    data->setData(QString::fromLatin1(inkType), Ink::encodeAll(strokes));
    // The clipboard owns it from here.
    QGuiApplication::clipboard()->setMimeData(data.release());
}

void InkSelection::cut() {
    if (!m_readOnly) {
        copy();
        remove();
    }
}

bool InkSelection::paste() {
    const QMimeData *data = inkOnClipboard();
    if (m_readOnly || data == nullptr) {
        return false;
    }
    auto strokes = Ink::decodeAll(data->data(QString::fromLatin1(inkType)));
    if (!strokes || strokes->empty()) {
        return false;
    }
    // Pasted where its strokes still are, it is set a little beside them, to be seen.
    const bool beside =
        std::ranges::any_of(*strokes, [this](const Ink::Stroke &stroke) { return m_lasso->sheet().find(stroke.id) != nullptr; });
    const QPointF by = beside ? QPointF(besideBy, besideBy) / m_scale : QPointF();
    edit(m_lasso->paste(std::move(*strokes), by));
    return true;
}

void InkSelection::marks(Ink::Mesh &mesh) const { Ink::marks(m_lasso->path(), m_lasso->box(), m_color, m_scale, lineWidth, mesh); }

bool InkSelection::eventFilter(QObject *watched, QEvent *event) {
    if (watched != m_surface || (event->type() != QEvent::KeyPress && event->type() != QEvent::ShortcutOverride)) {
        return false;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the event's type says its class, Qt's own idiom
    const auto &pressed = static_cast<const QKeyEvent &>(*event);
    const std::function<void()> action = actionOf(pressed);
    if (!action) {
        return false;
    }
    // Asked first whether a shortcut of the window may have the key: not while it is the selection's.
    if (event->type() == QEvent::KeyPress) {
        action();
    }
    event->accept();
    return true;
}

std::function<void()> InkSelection::actionOf(const QKeyEvent &event) {
    if (event.matches(QKeySequence::SelectAll)) {
        return [this] { selectAll(); };
    }
    if (event.matches(QKeySequence::Paste) && !m_readOnly && inkOnClipboard() != nullptr) {
        return [this] { std::ignore = paste(); };
    }
    if (count() == 0) {
        return {};
    }
    if (event.matches(QKeySequence::Copy)) {
        return [this] { copy(); };
    }
    if (event.key() == Qt::Key_Escape) {
        return [this] { clear(); };
    }
    if (m_readOnly) {
        return {};
    }
    if (event.matches(QKeySequence::Cut)) {
        return [this] { cut(); };
    }
    if (event.key() == Qt::Key_Delete || event.key() == Qt::Key_Backspace) {
        return [this] { remove(); };
    }
    const QPointF arrow = arrowOf(event.key()) * (event.modifiers().testFlag(Qt::ShiftModifier) ? bigStep : 1.0);
    if (arrow.isNull()) {
        return {};
    }
    return [this, arrow] { moveBy(arrow.x(), arrow.y()); };
}

void InkSelection::edit(Ink::Change change) {
    if (!change.empty()) {
        m_edit(std::move(change));
    }
}

void InkSelection::told() {
    const int now = count();
    const QRectF where = box();
    // A stroke recoloured is another stroke in the same place.
    if (now != m_shownCount || where != m_shownBox || ids() != m_shownIds) {
        // The lasso selecting takes the keyboard, for what is selected.
        if (m_shownCount == 0 && now > 0 && !m_readOnly) {
            m_surface->forceActiveFocus(Qt::OtherFocusReason);
        }
        m_shownCount = now;
        m_shownBox = where;
        m_shownIds = ids();
        emit changed();
    }
    m_surface->update();
}
