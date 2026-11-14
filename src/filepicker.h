#pragma once

#include <QList>
#include <QObject>
#include <QUrl>

// Asks the user for files and folders. The real one talks to the desktop
// portal; tests inject one that answers at once.
class FilePicker : public QObject {
    Q_OBJECT

public:
    enum Purpose { AddBooks, AddFolder, ChooseLibrary, ChooseCover, ExportBook, ImportReading, ExportReading,
                   // A plugin to install: as a .zip, or as the folder it is kept in.
                   InstallPlugin, InstallPluginFolder };
    Q_ENUM(Purpose)

    explicit FilePicker(QObject *parent = nullptr) : QObject(parent) {}

    // `suggested` is the file to propose when saving (ExportBook, ExportReading).
    virtual void pick(Purpose purpose, const QString &suggested = {}) = 0;

signals:
    void picked(FilePicker::Purpose purpose, const QList<QUrl> &urls);
    void failed(const QString &message);
};
