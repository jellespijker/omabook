// How the settings meet config.json (Settings, settings.h): reading it as it
// changes, writing it, and what is done while it can't be read. A file that
// can't be read is left exactly as it is (#222).
#include "settings.h"

#include <QFile>
#include <QFileInfo>
#include <atomic>
#include <tuple>
#include <utility>

#include "jsonfile.h"

using namespace Qt::StringLiterals;

namespace {

// Changes not written because the file couldn't be read, in the whole process.
[[nodiscard]] std::atomic_int &unsavedChanges() {
    static std::atomic_int count = 0;
    return count;
}

// What stands in for a config.json that can't be read from the start. Nothing
// is known of what the person chose, so nothing is asked of any model, no
// capability is allowed hosted, and no plugin's repository is asked by itself.
[[nodiscard]] QJsonObject cautious() {
    return {{"ai_provider", "off"}, {"capabilities", Capabilities::fresh()}, {"plugin_updates", "never"}};
}

// What is wrong is also written to config.error beside the file, as keys.json's
// and style.json's mistakes are (AGENTS.md there); gone once there is none.
void keepError(const QString &settingsFile, const std::optional<Failure> &mistake) {
    const QFileInfo settings(settingsFile);
    QFile file(settings.absolutePath() + '/' + settings.completeBaseName() + ".error");
    if (!mistake) {
        file.remove();
        return;
    }
    const QByteArray said = (mistake->english() + '\n').toUtf8();
    // Written only when it changes: the folder is watched.
    if (file.open(QIODevice::ReadOnly) && file.readAll() == said)
        return;
    file.close();
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(said);
}

} // namespace

int Settings::unsaved() { return unsavedChanges(); }

QString Settings::problem() const {
    if (!m_mistake)
        return {};
    return m_mistake->text() + ' '
           + (m_standingIn ? tr("Until it is fixed, Omabook uses its defaults, with AI off, and saves no settings.")
                           : tr("Until it is fixed, Omabook keeps the settings it had and saves no changes to them."));
}

void Settings::read() {
    const bool first = !std::exchange(m_looked, true);
    const auto held = JsonFile::read(m_file);
    std::optional<QJsonObject> values;
    if (!held) {
        // A file caught half-written, or with a typo, changes nothing that was read before.
        if (first) {
            values = cautious();
            m_standingIn = true;
        }
    } else if (*held) {
        values = upgraded(**held);
        m_standingIn = false;
    } else if (std::exchange(m_standingIn, false)) {
        // The unreadable file is gone: as at the first start of all.
        values = QJsonObject{{"capabilities", Capabilities::fresh()}};
    }
    const auto mistake = held ? std::nullopt : std::optional(held.error());
    keepError(m_file, mistake);
    const bool told = std::exchange(m_mistake, mistake) != mistake;
    if (values && *values != m_values) {
        m_values = *values;
        emit changed();
    }
    if (told)
        emit problemChanged();
    watch();
}

// A save replaces the file, which drops it from the watch.
void Settings::watch() {
    const QString folder = QFileInfo(m_file).absolutePath();
    if (QFileInfo::exists(m_file) && !m_watcher.files().contains(m_file))
        m_watcher.addPath(m_file);
    if (QFileInfo::exists(folder) && !m_watcher.directories().contains(folder))
        m_watcher.addPath(folder);
}

void Settings::save() {
    // Never over a file that can't be read, whether or not the change to it was
    // seen yet. One fixed meanwhile is read, and this change gives way to it.
    if (m_mistake || !JsonFile::read(m_file)) {
        ++unsavedChanges();
        read();
        return;
    }
    // A folder that can't be written keeps the choice for as long as this runs.
    std::ignore = JsonFile::write(m_file, upgraded(m_values));
    watch();
}
