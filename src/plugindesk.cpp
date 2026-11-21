#include "plugindesk.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QUrl>
#include <chrono>
#include <utility>

#include "filepicker.h"

namespace {

// How long after a key, a button or a touch the person counts as having just acted.
constexpr auto moment = std::chrono::seconds(3);

[[nodiscard]] bool pressed(const QEvent &event) {
    switch (event.type()) {
    case QEvent::KeyPress:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::TouchBegin:
    case QEvent::TabletPress:
        return true;
    default:
        return false;
    }
}

} // namespace

PluginDesk::PluginDesk(FilePicker *picker, QObject *parent) : QObject(parent), m_picker(picker) {
    if (QCoreApplication *application = QCoreApplication::instance())
        application->installEventFilter(this);
    connect(m_picker, &FilePicker::picked, this, [this](FilePicker::Purpose purpose, const QList<QUrl> &urls) {
        if (purpose != FilePicker::PluginFiles || !m_picked)
            return;
        QStringList paths;
        for (const QUrl &url : urls)
            if (url.isLocalFile())
                paths << url.toLocalFile();
        std::exchange(m_picked, nullptr)(paths);
    });
    // A dialog that never came, or was dismissed, is no file.
    connect(m_picker, &FilePicker::failed, this, [this] {
        if (m_picked)
            std::exchange(m_picked, nullptr)({});
    });
}

bool PluginDesk::eventFilter(QObject *watched, QEvent *event) {
    if (event && pressed(*event))
        m_input.start();
    return QObject::eventFilter(watched, event);
}

bool PluginDesk::justActed() const { return m_input.isValid() && m_input.durationElapsed() < moment; }

void PluginDesk::pickFiles(const QStringList &kinds, bool multiple, const std::function<void(const QStringList &)> &picked) {
    // One dialog at a time: a second plugin asking meanwhile gets nothing.
    if (m_picked) {
        picked({});
        return;
    }
    m_picked = picked;
    m_picker->pickForPlugin(kinds, multiple);
}

QString PluginDesk::clipboardText() const {
    const QClipboard *clipboard = QGuiApplication::clipboard();
    return clipboard ? clipboard->text() : QString();
}

void PluginDesk::setClipboardText(const QString &text) {
    if (QClipboard *clipboard = QGuiApplication::clipboard())
        clipboard->setText(text);
}

QImage PluginDesk::clipboardImage() const {
    const QClipboard *clipboard = QGuiApplication::clipboard();
    return clipboard ? clipboard->image() : QImage();
}

void PluginDesk::setClipboardImage(const QImage &image) {
    if (QClipboard *clipboard = QGuiApplication::clipboard())
        clipboard->setImage(image);
}
