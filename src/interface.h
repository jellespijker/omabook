#pragma once

#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickWindow>
#include <memory>

#include "customization.h"

// The window, loaded from the built-in views or from those with the user's
// own laid over, and loaded again whenever a view of theirs changes. A view
// that doesn't load is reported in ui.error and the built-in window comes up
// instead, so a mistake in a file never leaves the user without their books.
class Interface : public QObject {
    Q_OBJECT

public:
    Interface(QQmlEngine *engine, Customization *custom, QObject *parent = nullptr);
    ~Interface() override;

    // False only when not even the built-in window loads.
    bool load();
    QQuickWindow *window() const noexcept { return m_window; }

signals:
    // The window was loaded, for the first time or again.
    void loaded();

private:
    [[nodiscard]] std::unique_ptr<QObject> create(const QUrl &url, QString *problem);

    QQmlEngine *m_engine;
    Customization *m_custom;
    QPointer<QQuickWindow> m_window;
};
