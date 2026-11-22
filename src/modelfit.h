#pragma once

#include <QString>
#include <QStringList>
#include <QtTypes>
#include <cstdint>
#include <optional>

#include "aicapability.h"
#include "aiendpoint.h"

class Settings;

// Which model on a server fits a capability (#172), and which small,
// well-known one to fetch onto an Ollama server where none does: the
// knowledge behind "the server has no model for it", without the network
// (ModelScout asks the server, ModelHelp puts the two together).
namespace ModelFit {

// A model a server has, as it describes it: Ollama's /api/tags and
// /api/show, or only a name in an OpenAI-compatible /models.
struct Model {
    QString name;
    QString family;        // Ollama's details.family: "llama", "gemma3", "nomic-bert", "clip", ...
    QStringList families;  // details.families
    QStringList abilities; // /api/show's "capabilities": "completion", "vision", "embedding", ...
    qint64 bytes = 0;      // its size on the server, where it says
};

enum class Fit : std::uint8_t { Fits, Unfit, Unknown };

// Whether the model can serve the capability: by what the server says it
// does (`abilities`), else by its family and name. Unknown is never offered.
[[nodiscard]] Fit fit(Capability capability, const Model &model);

// A small, well-known model to download for the capability on Ollama, and
// its download size, from Ollama's library. Speech either way has none:
// Ollama serves neither, and this computer's ways (voxtype, whisper.cpp,
// Piper) are installed in their row. Kept up by hand; #175 finds more.
struct Suggestion {
    QString name;
    qint64 bytes = 0;
};
[[nodiscard]] std::optional<Suggestion> suggested(Capability capability);

// Whether two names are the same model on Ollama, which calls
// "nomic-embed-text" "nomic-embed-text:latest".
[[nodiscard]] bool same(const QString &one, const QString &other);

// Where a capability's model is asked for, when that is a server of the
// OpenAI-compatible API (which may be Ollama): the server and the model it is
// asked for. None for anything else (a harness, Gemini, voxtype, Piper), and
// with the AI Off, when nothing is asked of any server.
struct Place {
    AiServer server;
    QString model;
};
[[nodiscard]] std::optional<Place> placeOf(const Settings &settings, Capability capability);

// Ollama's own API is at the server's root, beside the OpenAI-compatible /v1.
[[nodiscard]] QString ollamaRoot(const AiServer &server);

// Chooses the model for the capability, in the key its provider is set up in
// (`ai_models`, `recognition`, `meaning`, `read_aloud`).
void choose(Settings &settings, Capability capability, const QString &model);

} // namespace ModelFit
