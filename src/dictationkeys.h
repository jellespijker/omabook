#pragma once

#include <QKeyCombination>
#include <QKeyEvent>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QString>

// The push-to-talk key of a field dictated into (#148), Ctrl+M: pressed,
// and let go (DictationButton.qml tells a key held while speaking from one
// pressed once to start and again to stop). It is caught before the field
// sees it, and only while the field has the keyboard, so it works in any
// text field: Omabook's own and a plugin view's. While the field is listened
// for, Escape is caught too: it stops listening, rather than closing the
// note before the words are in it. Every other key goes to the field as it
// came, voxtype's typing included.
class DictationKeys : public QObject {
    Q_OBJECT
    Q_PROPERTY(QQuickItem *target READ target WRITE setTarget NOTIFY targetChanged)
    Q_PROPERTY(bool catchEscape MEMBER m_catchEscape NOTIFY catchEscapeChanged)

public:
    // The key, as the help and the button's tooltip name it.
    static constexpr auto key = "Ctrl+M";

    explicit DictationKeys(QObject *parent = nullptr);

    [[nodiscard]] QQuickItem *target() const { return m_target; }
    void setTarget(QQuickItem *target);

signals:
    void pressed();
    void released();
    void escaped();
    void targetChanged();
    void catchEscapeChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    [[nodiscard]] bool ours(const QKeyEvent &key) const;
    [[nodiscard]] bool escape(const QKeyEvent &key) const;
    bool press(const QKeyEvent &key);
    bool release(const QKeyEvent &key);

    QPointer<QQuickItem> m_target;
    QKeyCombination m_key;
    bool m_catchEscape = false;
    bool m_down = false; // pressed, and not let go yet
};
