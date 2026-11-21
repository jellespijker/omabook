#pragma once

#include <QString>

class Settings;

// Whether what a plugin asks of the AI the person chose goes to their own
// computers or to someone else's (#138), as `omabook.ai.status` tells it, so
// a plugin that writes from private notes can keep to the person's own.
//
// The person's own are a server on this computer (Omarchy's local models,
// Ollama on localhost), on their tailnet, or at an address they marked as
// theirs (config.json's `recognition.own`), with a model that runs there; a
// model whose name says it runs elsewhere (`...:cloud`) does not. Claude's
// API and the agents (Claude Code, Codex, ...) are someone else's, and so
// is a command of the person's own: what it reaches can't be known here.
namespace AiReach {

// `model` is what `ai.ask` is given: "main" or "" for the main model,
// "quick", or a model by name.
[[nodiscard]] bool hosted(const Settings &settings, const QString &model);

} // namespace AiReach
