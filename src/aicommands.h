#pragma once

#include <QStringList>

// The `ai` command: which server answers when the AI is one (the "openai"
// choice), its models, and its key.
int runAiCli(const QStringList &arguments);
