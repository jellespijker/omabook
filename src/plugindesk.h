#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QStringList>
#include <functional>

#include "scriptdesk.h"

class FilePicker;

// The window's side of what plugins ask of the desktop (ScriptDesk, #131):
// the desktop's file dialog through the window's FilePicker, one request at a
// time, and the clipboard. It watches the window's input, so a plugin is
// held to the person having just pressed a key, a button or the screen.
class PluginDesk : public QObject, public ScriptDesk {
    Q_OBJECT

public:
    // Watches the whole application's input, so it is made once, by the window.
    explicit PluginDesk(FilePicker *picker, QObject *parent = nullptr);

    [[nodiscard]] bool justActed() const override;
    void pickFiles(const QStringList &kinds, bool multiple, const std::function<void(const QStringList &)> &picked) override;
    [[nodiscard]] QString clipboardText() const override;
    void setClipboardText(const QString &text) override;
    [[nodiscard]] QImage clipboardImage() const override;
    void setClipboardImage(const QImage &image) override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    FilePicker *m_picker;
    QElapsedTimer m_input; // since the person last pressed something
    std::function<void(const QStringList &)> m_picked;
};
