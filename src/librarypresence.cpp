#include "librarypresence.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <algorithm>
#include <mutex>

#include "jsonfile.h"
#include "library.h"
#include "settings.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QString spelled(const QString &folder) { return QDir::cleanPath(QDir(folder).absolutePath()); }

// The libraries this program has opened, asked about from any thread.
struct Opened {
    std::mutex mutex;
    QSet<QString> folders;
};

[[nodiscard]] Opened &openedHere() {
    static Opened opened;
    return opened;
}

} // namespace

bool LibraryPresence::there(const QString &folder) { return !folder.isEmpty() && QFileInfo::exists(Library::databaseIn(folder)); }

// As the file stands, not as Settings fills it in: the usual folder and a
// library named for one run (--library) are not written in it.
bool LibraryPresence::configured(const QString &folder) {
    const auto values = JsonFile::object(Settings::defaultFile());
    // One that can't be read (#222) may name any folder, so none is made.
    if (!values)
        return true;
    QJsonArray named = values->value("libraries"_L1).toArray();
    named.append(values->value("library"_L1));
    const QString asked = spelled(folder);
    return std::ranges::any_of(named,
                               [&asked](const QJsonValue &one) { return !one.toString().isEmpty() && spelled(one.toString()) == asked; });
}

bool LibraryPresence::missing(const QString &folder) {
    if (there(folder))
        return false;
    Opened &here = openedHere();
    {
        const std::scoped_lock lock(here.mutex);
        if (here.folders.contains(spelled(folder)))
            return true;
    }
    return configured(folder);
}

void LibraryPresence::opened(const QString &folder) {
    Opened &here = openedHere();
    {
        const std::scoped_lock lock(here.mutex);
        here.folders.insert(spelled(folder));
    }
    if (configured(folder))
        return;
    // Unnamed in config.json and yet the one in use: the usual folder.
    if (Settings settings; spelled(settings.libraryPath()) == spelled(folder))
        settings.setLibraryPath(folder);
}
