#include "hubplace.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <span>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>

#include "secretfile.h"

using namespace Qt::StringLiterals;

namespace {

constexpr auto folderName = "omabook-hub";
constexpr mode_t groupOrOthers = 0077U;
// The system variant's runtime folder, as its unit's RuntimeDirectory= makes it.
constexpr auto systemRuntime = "/run/omabook-hub";
// A secret is a word: never a path that leads out of the secrets' folder.
const QRegularExpression &secretName() {
    static const QRegularExpression name(u"^[a-z0-9][a-z0-9-]*$"_s);
    return name;
}

// The first of the folders systemd gives in `variable` (a list, split by ':').
[[nodiscard]] QString fromSystemd(const char *variable) { return qEnvironmentVariable(variable).section(':', 0, 0); }

[[nodiscard]] QString underXdg(const char *variable, const QString &otherwise) {
    const QString base = qEnvironmentVariableIsSet(variable) ? qEnvironmentVariable(variable) : otherwise;
    return QDir(base).filePath(QString::fromLatin1(folderName));
}

[[nodiscard]] bool mine(const QString &folder) {
    const QFileInfo info(folder);
    return info.isDir() && info.ownerId() == ::geteuid();
}

// The file whole, when it is a regular file of this user's that no one else may read.
[[nodiscard]] std::expected<QByteArray, Failure> readOwnersOnly(const QString &file) {
    const QByteArray path = QFile::encodeName(file);
    const int fd = ::open(path.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return std::unexpected(Failure(Failure::Code::HubSecretUnreadable, {file, qt_error_string(errno)}));
    QFile opened;
    if (!opened.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        std::ignore = ::close(fd); // Not read either way; the failure says so.
        return std::unexpected(Failure(Failure::Code::HubSecretUnreadable, {file, opened.errorString()}));
    }
    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != ::geteuid())
        return std::unexpected(Failure(Failure::Code::HubSecretUnreadable, {file, qt_error_string(EPERM)}));
    if ((info.st_mode & groupOrOthers) != 0)
        return std::unexpected(Failure(Failure::Code::HubSecretOpen, {file}));
    return opened.readAll();
}

} // namespace

HubPlace HubPlace::usual() {
    const QString home = QDir::homePath();
    const QString state = fromSystemd("STATE_DIRECTORY");
    QString runtime = fromSystemd("RUNTIME_DIRECTORY");
    if (runtime.isEmpty() && mine(QString::fromLatin1(systemRuntime)))
        runtime = QString::fromLatin1(systemRuntime);
    if (runtime.isEmpty() && qEnvironmentVariableIsSet("XDG_RUNTIME_DIR"))
        runtime = underXdg("XDG_RUNTIME_DIR", {});
    Folders folders{.state = state.isEmpty() ? underXdg("XDG_STATE_HOME", home + u"/.local/state"_s) : state,
                    .runtime = runtime,
                    .credentials = qEnvironmentVariable("CREDENTIALS_DIRECTORY")};
    if (folders.runtime.isEmpty())
        folders.runtime = folders.state;
    return HubPlace(std::move(folders));
}

QString HubPlace::socket() const { return QDir(m_folders.runtime).filePath(u"hub.sock"_s); }

QString HubPlace::choicesFile() const { return QDir(m_folders.state).filePath(u"hub.json"_s); }

QString HubPlace::libraries() const {
    QFile file(choicesFile());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object().value("libraries"_L1).toString();
}

std::expected<void, Failure> HubPlace::chooseLibraries(const QString &folder) const {
    const QString absolute = QDir(folder).absolutePath();
    if (!QDir().mkpath(absolute))
        return std::unexpected(Failure(Failure::Code::LibraryUnwritable, {absolute}));
    QJsonObject choices;
    if (QFile file(choicesFile()); file.open(QIODevice::ReadOnly))
        choices = QJsonDocument::fromJson(file.readAll()).object();
    choices.insert("libraries"_L1, absolute);
    QSaveFile file(choicesFile());
    if (!QDir().mkpath(m_folders.state) || !file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(choices).toJson()) < 0
        || !file.commit())
        return std::unexpected(Failure(Failure::Code::LibraryUnwritable, {choicesFile()}));
    return {};
}

std::expected<QByteArray, Failure> HubPlace::secret(const QString &name) const {
    if (!secretName().match(name).hasMatch())
        return std::unexpected(Failure(Failure::Code::HubSecretUnreadable, {name, qt_error_string(EINVAL)}));
    // systemd hands the decrypted credential to this service alone, in a
    // folder of its own: it is the hub's, whatever its mode says.
    if (const QString credential = QDir(m_folders.credentials).filePath(QString::fromLatin1(folderName) + '.' + name);
        !m_folders.credentials.isEmpty() && QFileInfo::exists(credential)) {
        QFile file(credential);
        if (!file.open(QIODevice::ReadOnly))
            return std::unexpected(Failure(Failure::Code::HubSecretUnreadable, {credential, file.errorString()}));
        return file.readAll();
    }
    return readOwnersOnly(QDir(m_folders.state).filePath(u"secrets/"_s + name));
}

std::expected<void, Failure> HubPlace::keepSecret(const QString &name, QByteArrayView bytes) const {
    const QString file = QDir(m_folders.state).filePath(u"secrets/"_s + name);
    if (!secretName().match(name).hasMatch())
        return std::unexpected(Failure(Failure::Code::HubSecretUnreadable, {name, qt_error_string(EINVAL)}));
    const auto kept = SecretFile::keep(file, std::as_bytes(std::span(bytes.data(), static_cast<std::size_t>(bytes.size()))),
                                       SecretFile::Placing::Replace);
    if (!kept)
        return std::unexpected(Failure::said(kept.error()));
    return {};
}
