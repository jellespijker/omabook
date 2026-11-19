#include "omarchylocal.h"

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <algorithm>

namespace {

using namespace Qt::StringLiterals;

// Where Omarchy keeps the keys its model servers take: one for this
// computer's, and one for each computer of the tailnet it is paired with.
QString keyFolder() { return QDir::homePath() + u"/.config/omarchy/agent-local"_s; }

// What `program <command> --json` prints.
std::expected<QJsonObject, QString> ask(const QString &program, const QString &command, std::chrono::milliseconds timeout) {
    QProcess process;
    process.start(program, {command, u"--json"_s});
    if (!process.waitForFinished(static_cast<int>(timeout.count()))) {
        process.kill();
        process.waitForFinished(1000);
        return std::unexpected(u"Omarchy's local models didn't answer: %1 %2 --json"_s.arg(program, command));
    }
    QJsonParseError parsed{};
    const QJsonDocument document = QJsonDocument::fromJson(process.readAllStandardOutput(), &parsed);
    if (process.exitCode() != 0 || parsed.error != QJsonParseError::NoError || !document.isObject())
        return std::unexpected(u"Omarchy's local models can't be read: %1 %2 --json failed"_s.arg(program, command));
    return document.object();
}

// The models this computer runs, by `status --json`: only one that is up
// can answer.
QList<OmarchyLocal::Model> here(const QJsonObject &status) {
    QList<OmarchyLocal::Model> models;
    const QJsonObject all = status.value("models"_L1).toObject();
    for (auto it = all.begin(); it != all.end(); ++it) {
        const QJsonObject model = it.value().toObject();
        if (model.value("unit"_L1).toString() != "active"_L1 || model.value("endpoint"_L1).toString().isEmpty())
            continue;
        const QString recipe = model.value("recipe"_L1).toString(it.key());
        models.append({.id = recipe,
                       .model = recipe,
                       .name = model.value("name"_L1).toString(recipe),
                       .host = {},
                       .baseUrl = model.value("endpoint"_L1).toString(),
                       .keyFile = keyFolder() + u"/key"_s});
    }
    return models;
}

// Those shared on the tailnet, by `network --json`: only one paired with
// this computer, on a computer that can be reached, and up.
QList<OmarchyLocal::Model> shared(const QJsonObject &network) {
    QList<OmarchyLocal::Model> models;
    const QJsonObject all = network.value("models"_L1).toObject();
    for (auto it = all.begin(); it != all.end(); ++it) {
        const QJsonObject model = it.value().toObject();
        const QString host = model.value("host"_L1).toString();
        if (!model.value("paired"_L1).toBool() || !model.value("offline"_L1).toString().isEmpty()
            || model.value("unit"_L1).toString() != "active"_L1 || host.isEmpty())
            continue;
        const QString recipe = model.value("recipe"_L1).toString();
        models.append({.id = it.key(),
                       .model = recipe,
                       .name = model.value("name"_L1).toString(recipe),
                       .host = host,
                       .baseUrl = model.value("endpoint"_L1).toString(),
                       .keyFile = keyFolder() + u"/remotes/"_s + host + u".key"_s});
    }
    return models;
}

} // namespace

QString OmarchyLocal::program() { return QStandardPaths::findExecutable(u"omarchy-agent-local"_s); }

bool OmarchyLocal::installed() { return !program().isEmpty(); }

std::expected<QList<OmarchyLocal::Model>, QString> OmarchyLocal::running(const QString &command, std::chrono::milliseconds timeout) {
    const QString found = command.isEmpty() ? program() : command;
    if (found.isEmpty())
        return std::unexpected(u"Omarchy's local models aren't installed here (there is no omarchy-agent-local command)."_s);
    const auto status = ask(found, u"status"_s, timeout);
    if (!status)
        return std::unexpected(status.error());
    QList<Model> models = here(*status);
    // The tailnet is surveyed only when this computer runs none: asking the
    // other computers takes a while.
    if (models.isEmpty()) {
        if (const auto network = ask(found, u"network"_s, timeout))
            models = shared(*network);
    }
    return models;
}

std::expected<OmarchyLocal::Model, QString> OmarchyLocal::pick(const QList<Model> &models, const QString &wanted) {
    if (models.isEmpty())
        return std::unexpected(u"No Omarchy local model is running. Start one with: omarchy agent local use best"_s);
    const auto named = std::ranges::find_if(models, [&wanted](const Model &model) { return model.id == wanted || model.model == wanted; });
    return named != models.end() ? *named : models.first();
}
