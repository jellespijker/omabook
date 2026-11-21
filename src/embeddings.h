#pragma once

#include <QString>
#include <QStringList>
#include <chrono>
#include <cstdint>
#include <expected>
#include <vector>

#include "aiendpoint.h"
#include "failure.h"

class Settings;

// Embeddings of text, made by a server of the OpenAI-compatible API (POST
// /embeddings), for finding a book's text by what it means (#72): Ollama's
// with a local model such as nomic-embed-text, by default the AI server
// itself. A book's text goes by the privacy rule handwriting and speech go by
// (#84): only to the person's own servers (this computer, the tailnet, an
// address they marked as theirs), unless they allowed hosted ones for it in
// the settings; and then they are told where it goes.
namespace Embeddings {

inline constexpr auto defaultModel = "nomic-embed-text";

// Where the embeddings are made, and whether that is someone else's computer.
struct Route {
    AiServer server;
    QString model;
    bool hosted = false;
};
// The route the settings choose ("meaning" in config.json; Settings::meaning).
// Fails when there is no server, or the one there is may not be sent a book's text.
[[nodiscard]] std::expected<Route, Failure> route(const Settings &settings, const QString &omarchyProgram = {});

// What is embedded: a stretch of a book, or a question asked of it. Some
// models are taught to tell the two apart by a word in front.
enum class Use : std::uint8_t { Document, Question };
[[nodiscard]] QString asked(const QString &model, const QString &text, Use use);

using Vectors = std::vector<std::vector<float>>;
// The embeddings of the texts, in their order, each of length 1, waiting for
// the answer at most `timeout`: for a thread of its own or the command line.
[[nodiscard]] std::expected<Vectors, Failure> embed(const Route &route, const QStringList &texts, Use use,
                                                    std::chrono::milliseconds timeout = std::chrono::minutes(2));

} // namespace Embeddings
