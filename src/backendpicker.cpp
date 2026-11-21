// The window's side of the file picker (filepicker.h): what Backend asks it
// for, and what it does with what was picked. The plugin manager and the
// space desk ask for, and take, their own.

#include "backend.h"

#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QStandardPaths>

void Backend::addBooks() { m_picker->pick(FilePicker::AddBooks); }
void Backend::addFolder() { m_picker->pick(FilePicker::AddFolder); }
void Backend::chooseLibrary() { m_picker->pick(FilePicker::ChooseLibrary); }

void Backend::chooseCover(qint64 id) {
    m_pickingFor = id;
    m_picker->pick(FilePicker::ChooseCover);
}

void Backend::exportBook(qint64 id) {
    const QString file = m_library ? m_library->formatPath(id) : QString();
    if (file.isEmpty())
        return;
    m_pickingFor = id;
    m_picker->pick(FilePicker::ExportBook,
                   QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) + "/" + QFileInfo(file).fileName());
}

void Backend::pickReadingFile(bool save, const QString &suggested) {
    m_picker->pick(save ? FilePicker::ExportReading : FilePicker::ImportReading, suggested);
}

void Backend::useCover(const QString &file) {
    QImageReader reader(file);
    reader.setAutoTransform(true);
    const QImage cover = reader.read();
    if (cover.isNull() || !m_library || !m_library->setCover(m_pickingFor, cover))
        say(tr("That picture can't be used as a cover."));
    reload();
}

void Backend::saveCopy(const QString &target) {
    const QString source = m_library ? m_library->formatPath(m_pickingFor) : QString();
    // The save dialog has already asked about replacing an existing file.
    if (QFileInfo(target) != QFileInfo(source))
        QFile::remove(target);
    if (source.isEmpty() || !QFile::copy(source, target))
        say(tr("The copy can't be saved to %1.").arg(target));
    else
        say(tr("Saved a copy to %1.").arg(target));
}

void Backend::picked(FilePicker::Purpose purpose, const QList<QUrl> &urls) {
    switch (purpose) {
    case FilePicker::AddBooks:
    case FilePicker::AddFolder:
        addUrls(urls);
        break;
    case FilePicker::ChooseLibrary:
        useLibrary(urls.first().toLocalFile());
        break;
    case FilePicker::ChooseCover:
        useCover(urls.first().toLocalFile());
        break;
    case FilePicker::ExportBook:
        saveCopy(urls.first().toLocalFile());
        break;
    case FilePicker::ImportReading:
    case FilePicker::ExportReading:
        emit readingFilePicked(purpose == FilePicker::ExportReading, urls.first().toLocalFile());
        break;
    case FilePicker::InstallPlugin:
    case FilePicker::InstallPluginFolder:
    case FilePicker::SaveSpace:
    case FilePicker::OpenSpace:
    case FilePicker::PluginFiles:
    case FilePicker::PluginSave:
        // The plugin manager's, the space desk's and the plugins' desk's, which listen to the picker themselves.
        break;
    }
}
