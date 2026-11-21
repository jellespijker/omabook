#pragma once

#include <QList>
#include <QObject>
#include <QStringList>
#include <QUrl>

// Asks the user for files and folders. The real one talks to the desktop
// portal; tests inject one that answers at once.
class FilePicker : public QObject {
    Q_OBJECT

public:
    enum Purpose { AddBooks, AddFolder, ChooseLibrary, ChooseCover, ExportBook, ImportReading, ExportReading,
                   // A plugin to install: as a .zip, or as the folder it is kept in.
                   InstallPlugin, InstallPluginFolder,
                   // A published space's snapshot (.omabook-space): where to write one, and one to subscribe to.
                   SaveSpace, OpenSpace,
                   // Files a plugin asked the person for (omabook.ui.pickFile, #131).
                   PluginFiles,
                   // Where to save what a plugin made (omabook.ui.saveFile, #138).
                   PluginSave };
    Q_ENUM(Purpose)

    explicit FilePicker(QObject *parent = nullptr) : QObject(parent) {}

    // `suggested` is the file to propose when saving (ExportBook, ExportReading, SaveSpace).
    virtual void pick(Purpose purpose, const QString &suggested = {}) = 0;
    // Files for a plugin, of `kinds` ("image", "audio", "video", "text",
    // "pdf", a MIME type or an extension such as ".png"; any when empty), one
    // or `multiple`: answered as `picked` with PluginFiles, or with no files.
    virtual void pickForPlugin([[maybe_unused]] const QStringList &kinds, [[maybe_unused]] bool multiple) { pick(PluginFiles); }
    // Where to save a file a plugin made, named `name` to start with, of
    // `kinds` as above: answered as `picked` with PluginSave, or `dismissed`.
    virtual void saveForPlugin(const QString &name, [[maybe_unused]] const QStringList &kinds) { pick(PluginSave, name); }

signals:
    void picked(FilePicker::Purpose purpose, const QList<QUrl> &urls);
    void failed(const QString &message);
    // The person closed the dialog without choosing anything.
    void dismissed(FilePicker::Purpose purpose);
};
