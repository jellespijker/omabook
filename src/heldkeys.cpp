#include "heldkeys.h"

#include <QKeyEvent>
#include <QKeySequence>
#include <algorithm>

HeldKeys::HeldKeys(QObject *parent) : QObject(parent) {}

void HeldKeys::setWindow(QWindow *window) {
    if (window == m_window)
        return;
    if (m_window)
        m_window->removeEventFilter(this);
    m_window = window;
    if (m_window)
        m_window->installEventFilter(this);
    emit windowChanged();
}

bool HeldKeys::eventFilter(QObject *watched, QEvent *event) {
    if (watched != m_window || event->type() != QEvent::KeyRelease)
        return false;
    const auto *key = dynamic_cast<const QKeyEvent *>(event);
    if (!key || key->isAutoRepeat())
        return false;
    const bool ours = std::ranges::any_of(m_keys, [&](const QString &written) {
        const QKeySequence sequence(written);
        return !sequence.isEmpty() && sequence[0].key() == key->key();
    });
    if (ours)
        emit released();
    return false;
}
