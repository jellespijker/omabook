#pragma once

#include <QByteArray>
#include <QJSValue>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <deque>
#include <optional>

class QJSEngine;
class ScriptPlugin;

// The C++ side of `omabook.files` and of what of `omabook.ui` reaches the
// person's files and clipboard (#131), built in scriptfiles.js, which the
// script sees and this never is.
//
// A plugin reads its own folder and writes under its data/ folder, text or
// bytes (an ArrayBuffer, a typed array, base64), a file at most
// PluginFiles::maxFileBytes and all of them together at most
// PluginFiles::maxPluginBytes; a path that leads anywhere else throws. A
// file it removes is removed on the person's other computers too (PluginFiles).
//
// A file the person picks (`ui.pickFile`, with the `files` permission) and a
// picture on the clipboard (`ui.clipboard`, with `clipboard`) are held here
// and read through a handle, never by their path; asking for either, and the
// clipboard's text, only works right after the person did something of the
// plugin's (ScriptPlugin::acting), and where there is no window (a command)
// nothing is picked and the clipboard is empty.
class ScriptFiles : public QObject {
    Q_OBJECT

public:
    explicit ScriptFiles(ScriptPlugin *plugin) : m_plugin(plugin) {}

    // What scriptfiles.js makes of this: {files, ui}, for scriptprelude.js.
    [[nodiscard]] QJSValue build(QJSEngine &engine);

    Q_INVOKABLE QJSValue read(const QString &path);
    Q_INVOKABLE QJSValue readBytes(const QString &path);
    Q_INVOKABLE QJSValue readBase64(const QString &path);
    Q_INVOKABLE bool write(const QString &path, const QString &text);
    Q_INVOKABLE bool writeBytes(const QString &path, const QByteArray &bytes);
    Q_INVOKABLE bool writeBase64(const QString &path, const QString &base64);
    Q_INVOKABLE QString list(const QString &folder);
    Q_INVOKABLE bool exists(const QString &path);
    Q_INVOKABLE bool remove(const QString &path);
    Q_INVOKABLE QString usage();
    // On demand (#135): a path under data/ (a file or a folder) marked, or no
    // longer; what is known of a file, {here, elsewhere, onDemand}; and the
    // file fetched, `done` given whether it is here then, and why not.
    Q_INVOKABLE bool setOnDemand(const QString &path, bool onDemand);
    Q_INVOKABLE QString status(const QString &path);
    Q_INVOKABLE void fetch(const QString &path, const QJSValue &done);

    // `kinds` is JSON; `answered` is given JSON: a list of what hold() describes.
    // Throws at once when the plugin may not ask the person for a file now.
    Q_INVOKABLE void pick(const QString &kinds, bool multiple, const QJSValue &answered);
    Q_INVOKABLE QString clipboardText();
    Q_INVOKABLE bool setClipboardText(const QString &text);
    // JSON: what hold() describes, or null.
    Q_INVOKABLE QString clipboardImage();
    // A picture from a file of the plugin's folder (`path`), or else `bytes`.
    Q_INVOKABLE bool setClipboardImage(const QString &path, const QByteArray &bytes);

    // What is held by a handle's number.
    Q_INVOKABLE QJSValue heldBytes(int id);
    Q_INVOKABLE QJSValue heldBase64(int id);
    Q_INVOKABLE QJSValue heldText(int id);
    Q_INVOKABLE double heldCopy(int id, const QString &path);

private:
    struct Held {
        int id = 0;
        QString path;     // a file the person picked
        QByteArray bytes; // or what it is when it isn't a file
    };

    [[nodiscard]] bool allowed(const QString &permission) const;
    // Throws, and answers false, unless the person just did something of the plugin's.
    [[nodiscard]] bool acting(const QString &what);
    // A path of the plugin's to write, or empty with the script told why.
    [[nodiscard]] QString writable(const QString &path);
    // The same as the change log names it: under data/, as "pictures/one.png".
    [[nodiscard]] QString underData(const QString &path);
    // Writes `bytes` as `path`, held to the limits; false with the script told why.
    bool put(const QString &path, const QByteArray &bytes);
    // A handle's description: {id, name, size, type, width, height}.
    [[nodiscard]] QJsonObject hold(Held held, const QString &name);
    // What a file of the plugin's holds, or none: with the script told why
    // when it is out of bounds.
    [[nodiscard]] std::optional<QByteArray> bytesOf(const QString &path);
    // What a handle holds, or none with the script told why.
    [[nodiscard]] std::optional<QByteArray> contents(int id);
    void fail(const QString &why);

    ScriptPlugin *m_plugin;
    std::deque<Held> m_held;
    int m_next = 1;
};
