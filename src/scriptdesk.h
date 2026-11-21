#pragma once

#include <QImage>
#include <QString>
#include <QStringList>
#include <functional>

// What a plugin may ask of the person's desktop (#131): files they pick,
// where to save one it made (#138), and the clipboard. The window gives one (PluginDesk); a command has none, so
// there a plugin is picked no file and finds the clipboard empty.
//
// It also says whether the person just did something in the window, a key,
// a button, a touch: a plugin reaches their files and clipboard only right
// after they did something of its own (ScriptPlugin::acting).
class ScriptDesk {
public:
    ScriptDesk() = default;
    virtual ~ScriptDesk() = default;
    ScriptDesk(const ScriptDesk &) = delete;
    ScriptDesk &operator=(const ScriptDesk &) = delete;
    ScriptDesk(ScriptDesk &&) = delete;
    ScriptDesk &operator=(ScriptDesk &&) = delete;

    [[nodiscard]] virtual bool justActed() const = 0;
    // Asks the person for files of `kinds` ("image", "audio", "video",
    // "text", "pdf", or an extension such as ".png"; any when empty), one or
    // `multiple`, and calls `picked` with their paths: none when they chose none.
    virtual void pickFiles(const QStringList &kinds, bool multiple, const std::function<void(const QStringList &)> &picked) = 0;
    // Asks the person where to save a file named `name` to start with, of
    // `kinds` as above, and calls `chosen` with its path: empty when they
    // chose none, as it is where nobody can be asked.
    virtual void saveFile([[maybe_unused]] const QString &name, [[maybe_unused]] const QStringList &kinds,
                          const std::function<void(const QString &)> &chosen) {
        chosen({});
    }
    [[nodiscard]] virtual QString clipboardText() const = 0;
    virtual void setClipboardText(const QString &text) = 0;
    // A null image when there is none on the clipboard.
    [[nodiscard]] virtual QImage clipboardImage() const = 0;
    virtual void setClipboardImage(const QImage &image) = 0;
};
