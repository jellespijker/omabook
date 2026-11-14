#include "interface.h"

#include <QQmlComponent>
#include <QQmlContext>

Interface::Interface(QQmlEngine *engine, Customization *custom, QObject *parent)
    : QObject(parent), m_engine(engine), m_custom(custom) {
    // Queued: the files are still being written when the first change is seen.
    connect(custom, &Customization::viewsChanged, this, &Interface::load, Qt::QueuedConnection);
}

Interface::~Interface() { delete m_window; }

std::unique_ptr<QObject> Interface::create(const QUrl &url, QString *problem) {
    QQmlComponent component(m_engine, url);
    std::unique_ptr<QObject> root(component.isError() ? nullptr : component.create());
    if (!root) {
        QStringList errors;
        for (const QQmlError &error : component.errors())
            errors << error.toString();
        *problem = errors.join('\n');
    }
    return root;
}

bool Interface::load() {
    // Where the window was, so a reload doesn't move it.
    const QRect geometry = m_window ? m_window->geometry() : QRect();
    delete m_window;
    m_engine->clearComponentCache();

    QString problem;
    const QUrl url = m_custom->windowUrl(&problem);
    std::unique_ptr<QObject> root = problem.isEmpty() ? create(url, &problem) : nullptr;
    const bool own = url.scheme() != "qrc";
    // With no views of the user's own there is nothing to be wrong.
    m_custom->reportUi(own ? problem : QString());
    if (!root && own) {
        m_engine->clearComponentCache();
        QString builtInProblem;
        root = create(QUrl("qrc:/Main.qml"), &builtInProblem);
    }
    // Anything but a window is no use, and goes.
    if (!qobject_cast<QQuickWindow *>(root.get()))
        return false;
    m_window = static_cast<QQuickWindow *>(root.release());
    if (geometry.isValid())
        m_window->setGeometry(geometry);
    emit loaded();
    return true;
}
