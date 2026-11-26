#include "jsonfile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QSaveFile>
#include <mutex>

#include "log.h"

using namespace Qt::StringLiterals;

namespace {

// A file that stays unreadable is read many times over (every Settings, every
// change in its folder): the log is told once, and again if what is wrong changes.
void tellOnce(const QString &path, const QString &problem) {
    static std::mutex lock;
    static QHash<QString, QString> told;
    const std::scoped_lock locked(lock);
    if (problem.isEmpty()) {
        told.remove(path);
    } else if (told.value(path) != problem) {
        told.insert(path, problem);
        qCWarning(lcApp).noquote() << problem;
    }
}

[[nodiscard]] std::expected<std::optional<QJsonObject>, Failure> parsed(const QString &path) {
    if (!QFileInfo::exists(path))
        return std::nullopt;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::unexpected(Failure(Failure::Code::SettingsFileUnopened, {path, file.errorString()}));
    const QByteArray contents = file.readAll();
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(contents, &error);
    if (error.error != QJsonParseError::NoError) {
        const auto line = contents.first(std::min<qsizetype>(error.offset, contents.size())).count('\n') + 1;
        return std::unexpected(Failure(Failure::Code::SettingsFileMistaken, {path, QString::number(line), error.errorString()}));
    }
    if (!document.isObject())
        return std::unexpected(Failure(Failure::Code::SettingsFileNotObject, {path}));
    return document.object();
}

} // namespace

std::expected<std::optional<QJsonObject>, Failure> JsonFile::read(const QString &path) {
    auto held = parsed(path);
    tellOnce(path, held ? QString() : held.error().english());
    return held;
}

std::expected<QJsonObject, Failure> JsonFile::object(const QString &path) {
    return read(path).transform([](const std::optional<QJsonObject> &held) { return held.value_or(QJsonObject()); });
}

std::expected<void, Failure> JsonFile::write(const QString &path, const QJsonObject &object) {
    if (const auto held = read(path); !held)
        return std::unexpected(held.error());
    const bool folder = QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    // No rewriting in place: a folder that can't hold the temporary file fails
    // the write instead of risking a truncated one.
    file.setDirectWriteFallback(false);
    const QByteArray contents = QJsonDocument(object).toJson();
    if (!folder || !file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() || !file.commit())
        return std::unexpected(Failure(Failure::Code::BookFileUnwritable, {path, file.errorString()}));
    return {};
}
