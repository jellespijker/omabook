#pragma once

#include <QList>
#include <QString>
#include <chrono>
#include <expected>

// Omarchy's own local models (`omarchy agent local`): the open models it runs
// on this computer, and those other computers on the tailnet share with it.
// Each one is an OpenAI-compatible server of its own. What they are is read
// from Omarchy's command, `omarchy-agent-local`; nothing else in Omabook knows
// about that command or where Omarchy keeps its keys.
namespace OmarchyLocal {

struct Model {
    QString id;      // the recipe ("qwen3.8-27b-gufo"), or "host/recipe" for one shared on the tailnet
    QString model;   // the name the server takes it under: the recipe
    QString name;    // what Omarchy calls it
    QString host;    // the computer it runs on; empty for this one
    QString baseUrl; // ".../v1"
    // The file the key is in, read again for every request: Omarchy may
    // make a new one at any time.
    QString keyFile;
};

// Omarchy's command, found on PATH; empty when Omarchy's local models aren't installed.
[[nodiscard]] QString program();
// Whether Omarchy can run local models here at all.
[[nodiscard]] bool installed();

// The models running now: this computer's first, then the ones paired from
// other computers on the tailnet that are up. Asked of `command` (tests pass
// a stand-in), waiting at most `timeout` for each answer. Fails, in English,
// when the command can't be run or says nothing Omabook can read.
[[nodiscard]] std::expected<QList<Model>, QString> running(const QString &command = {},
                                                           std::chrono::milliseconds timeout = std::chrono::seconds(10));

// The one to ask: the running one named (by its id or its model), else the
// first. Fails, saying why, when none is running.
[[nodiscard]] std::expected<Model, QString> pick(const QList<Model> &models, const QString &wanted);

} // namespace OmarchyLocal
