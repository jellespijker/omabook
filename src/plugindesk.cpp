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
        QStringList paths;
        for (const QUrl &url : urls)
            if (url.isLocalFile())
                paths << url.toLocalFile();
        if (purpose == FilePicker::PluginFiles && m_picked)
            std::exchange(m_picked, nullptr)(paths);
        else if (purpose == FilePicker::PluginSave && m_saved)
            std::exchange(m_saved, nullptr)(paths.value(0));
    });
    // A dialog that never came, or was dismissed, is no file.
    connect(m_picker, &FilePicker::failed, this, [this] { answerNone(); });
    connect(m_picker, &FilePicker::dismissed, this, [this](FilePicker::Purpose purpose) {
        if (purpose == FilePicker::PluginFiles || purpose == FilePicker::PluginSave)
            answerNone();
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

void PluginDesk::saveFile(const QString &name, const QStringList &kinds, const std::function<void(const QString &)> &chosen) {
    if (m_saved) {
        chosen({});
        return;
    }
    m_saved = chosen;
    m_picker->saveForPlugin(name, kinds);
}

void PluginDesk::answerNone() {
    if (m_picked)
        std::exchange(m_picked, nullptr)({});
    if (m_saved)
        std::exchange(m_saved, nullptr)({});
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
