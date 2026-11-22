#pragma once

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QWindow>

// When a key that started something is let go, wherever the keyboard is in
// the window (#150): a Shortcut is told only that a key was pressed, so a key
// held while speaking, push to talk, needs its release from here. `keys`
// are as keys.json writes them ("Ctrl+M"); a key's release is told whichever
// modifier was let go first, and never for a key held down that repeats.
// Nothing is taken from the window: every key goes on as it came.
class HeldKeys : public QObject {
    Q_OBJECT
    Q_PROPERTY(QWindow *window READ window WRITE setWindow NOTIFY windowChanged)
    Q_PROPERTY(QStringList keys MEMBER m_keys NOTIFY keysChanged)

public:
    explicit HeldKeys(QObject *parent = nullptr);

    [[nodiscard]] QWindow *window() const { return m_window; }
    void setWindow(QWindow *window);

signals:
    void released();
    void windowChanged();
    void keysChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QPointer<QWindow> m_window;
    QStringList m_keys;
};
