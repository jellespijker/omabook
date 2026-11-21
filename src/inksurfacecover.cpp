// What lies over an ink surface where the pen comes down (#141): a button, a
// menu, a pane or a popup drawn over it takes the pen there, as it takes a
// finger and the mouse, and the surface writes nothing. The surface hears the
// pen before any item does (its filter of the application's events), so it
// asks the window what is drawn over that point first.

#include <QInputDevice>
#include <QQuickWindow>

#include <algorithm>

#include "inksurface.h"

namespace {

// A popup is open that takes every press in the window, inside it or out.
bool modalPopupIn(const QQuickItem &overlay) {
    return std::ranges::any_of(overlay.childItems(), [](const QQuickItem *child) {
        const QObject *popup = child->parent();
        return child->isVisible() && popup != nullptr && popup->inherits("QQuickPopup") && popup->property("modal").toBool();
    });
}

// Whether an item takes a pen's press itself: a pointer handler of its that
// is pressed does, by a pen; or, with none such (an item with handlers takes
// the mouse's buttons for them), it takes the mouse's button (a button, a mouse
// area, a popup). A tool tip takes nothing, and the overlay of popups only
// while one of them is modal. An ink surface is no cover for another: which
// of two takes the pen is the order they hear it in.
bool takesPen(const QQuickItem &item) {
    if (qobject_cast<const InkSurface *>(&item) != nullptr || item.inherits("QQuickToolTip")
        || (item.parent() != nullptr && item.parent()->inherits("QQuickToolTip"))) {
        return false;
    }
    if (item.inherits("QQuickOverlay")) {
        return modalPopupIn(item);
    }
    // The handlers that take a press: not those of a pointer that rests or turns a wheel.
    const auto presses = [](const QObject *child) {
        return child->inherits("QQuickPointerHandler") && !child->inherits("QQuickHoverHandler") && !child->inherits("QQuickWheelHandler");
    };
    const QObjectList &children = item.children();
    if (std::ranges::none_of(children, presses)) {
        return (item.acceptedMouseButtons() & Qt::LeftButton) != 0;
    }
    return std::ranges::any_of(children, [&presses](const QObject *child) {
        return presses(child) && child->property("enabled").toBool()
               && child->property("acceptedDevices").value<QInputDevice::DeviceTypes>().testFlag(QInputDevice::DeviceType::Stylus);
    });
}

// The items at a point of the scene in the order the window draws them,
// walked until one drawn after `surface` takes the pen.
class Cover {
public:
    Cover(const QQuickItem &surface, QPointF scene) : m_surface(&surface), m_scene(scene) {}

    [[nodiscard]] bool over(const QQuickItem &root) {
        walk(root);
        return m_covered;
    }

private:
    void walk(const QQuickItem &item) {
        if (m_covered || !item.isVisible() || !item.isEnabled() || item.opacity() <= 0.0) {
            return;
        }
        const QPointF local = item.mapFromScene(m_scene);
        const bool inside = item.contains(local);
        if (item.clip() && !inside) {
            return;
        }
        // Children below the item first (z under 0), then the item, then the rest.
        QList<QQuickItem *> children = item.childItems();
        std::ranges::stable_sort(children, {}, &QQuickItem::z);
        const auto above = std::ranges::find_if(children, [](const QQuickItem *child) { return child->z() >= 0; });
        std::for_each(children.begin(), above, [this](const QQuickItem *child) { walk(*child); });
        if (&item == m_surface) {
            m_reached = true;
        } else if (m_reached && inside && !isInSurface(item) && takesPen(item)) {
            m_covered = true;
        }
        std::for_each(above, children.end(), [this](const QQuickItem *child) { walk(*child); });
    }
    [[nodiscard]] bool isInSurface(const QQuickItem &item) const { return m_surface->isAncestorOf(&item); }

    const QQuickItem *m_surface;
    QPointF m_scene;
    bool m_reached = false;
    bool m_covered = false;
};

} // namespace

bool InkSurface::coveredAt(QPointF scene) const {
    const QQuickWindow *shown = window();
    return shown != nullptr && Cover(*this, scene).over(*shown->contentItem());
}
