#include "dictationkeys.h"

#include <QKeyEvent>
#include <QKeySequence>

DictationKeys::DictationKeys(QObject *parent) : QObject(parent), m_key(QKeySequence(QString::fromLatin1(key))[0]) {}

void DictationKeys::setTarget(QQuickItem *target) {
    if (target == m_target)
        return;
    if (m_target)
        m_target->removeEventFilter(this);
    m_target = target;
    m_down = false;
    if (m_target)
        m_target->installEventFilter(this);
    emit targetChanged();
}

bool DictationKeys::ours(const QKeyEvent &key) const {
    return key.key() == m_key.key() && (key.modifiers() & ~Qt::KeypadModifier) == m_key.keyboardModifiers();
}

bool DictationKeys::escape(const QKeyEvent &key) const { return m_catchEscape && key.key() == Qt::Key_Escape; }

bool DictationKeys::eventFilter(QObject *watched, QEvent *event) {
    const QEvent::Type type = event->type();
    if (watched != m_target || (type != QEvent::ShortcutOverride && type != QEvent::KeyPress && type != QEvent::KeyRelease))
        return false;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): these three types are QKeyEvents, as Qt's event filters tell them
    const auto &key = static_cast<const QKeyEvent &>(*event);
    if (type == QEvent::KeyPress)
        return press(key);
    if (type == QEvent::KeyRelease)
        return release(key);
    // Kept from the window's shortcuts, as the field keeps its own keys.
    if (!ours(key) && !escape(key))
        return false;
    event->accept();
    return true;
}

bool DictationKeys::press(const QKeyEvent &key) {
    if (ours(key)) {
        if (!key.isAutoRepeat() && !m_down) {
            m_down = true;
            emit pressed();
        }
        return true;
    }
    if (!escape(key))
        return false;
    emit escaped();
    return true;
}

bool DictationKeys::release(const QKeyEvent &key) {
    // Let go of the letter, whether the Ctrl was let go before it or not.
    if (key.key() != m_key.key() || !m_down || key.isAutoRepeat())
        return false;
    m_down = false;
    emit released();
    return true;
}
