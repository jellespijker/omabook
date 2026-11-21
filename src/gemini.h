#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <vector>

#include "aicapability.h"
#include "failure.h"
#include "recognition.h"

class QNetworkReply;
class SecretStore;
class Settings;
struct AiRequest;

// Google's Gemini API with a key of the person's own (#145, ADR 0008): one
// provider that serves all six capabilities, each through its own class
// (GeminiProvider for answers and quick, GeminiVision and GeminiTranscription
// for handwriting and speech, GeminiVoice for reading aloud) and Gemini::embed
// for the meaning index. This is what they share: where the API is, its key,
// the models, and the shapes of what is sent and answered, as plain data so
// they are tested without a network.
//
// It is always someone else's computer: used only for a capability the person
// allowed hosted for (AiPrivacy).
//
// The request shapes are Gemini's own REST API (v1beta), as Google's API
// documentation had them when checked on 2026-11-21:
//   POST {base}/models/{model}:streamGenerateContent?alt=sse   answers, as server-sent events
//   POST {base}/models/{model}:generateContent                  handwriting, speech, voices
//   POST {base}/models/{model}:batchEmbedContents               embeddings
// with the key in the `x-goog-api-key` header; errors are
// {"error": {"code", "message", "status", "details"}}.
//
// config.json's "gemini" (optional; the key is in the keyring, never here):
//   "url"      where the API is, for a proxy or a test (".../v1beta" by default)
//   "models"   a model per capability by its name ("handwriting", "speech-to-text",
//              "text-to-speech", "embeddings"); answers' and quick's are in
//              `ai_models` under "gemini", as every answering provider's are
namespace Gemini {

inline constexpr auto defaultUrl = "https://generativelanguage.googleapis.com/v1beta";
// The secret's name in the keyring, and the variable read when it has none.
inline constexpr auto secretName = "gemini";
inline constexpr auto keyVariable = "GEMINI_API_KEY";

// The models used when none is chosen, by Google's model list of 2026-11-21:
// Gemini 4 Hydrogen and 3.5 Flash-Lite are what it recommends for new work,
// 3.8 Flash the stable one that reads pictures, 3.5 Transcribe its
// speech-to-text model (each word with its moment), 3.8 Flash TTS its voice
// and gemini-embedding-001 the text embedding model that takes a task type.
inline constexpr auto answersModel = "gemini-4-hydrogen";
inline constexpr auto quickModel = "gemini-3.5-flash-lite";
inline constexpr auto visionModel = "gemini-3.8-flash";
inline constexpr auto transcriptionModel = "gemini-3.5-transcribe";
inline constexpr auto voiceModel = "gemini-3.8-flash-tts";
inline constexpr auto embeddingModel = "gemini-embedding-001";
inline constexpr auto defaultVoice = "Kore";
// The most of a recording sent at once: a request may be 20 MB in all, and
// the recording goes in it as base64, a third larger.
inline constexpr qsizetype largestRecording = qsizetype{14} * 1024 * 1024;

// Where it is, and the key it is asked with.
struct Api {
    QString baseUrl;
    QString key;
    // The host, as a failure names it.
    [[nodiscard]] QString host() const;
};

// The API as the settings have it; fails with GeminiNoKey when no key is
// kept in the keyring (nor in GEMINI_API_KEY).
[[nodiscard]] std::expected<Api, Failure> api(const Settings &settings);
[[nodiscard]] bool hasKey();
// Keeps the key in the keyring; an empty one forgets it. Fails with why.
[[nodiscard]] std::expected<void, Failure> keepKey(const QString &key, SecretStore &store);

// The model chosen for a capability, else its default.
[[nodiscard]] QString modelFor(const Settings &settings, Capability capability);
[[nodiscard]] QString modelDefault(Capability capability);
// The model as the person chose it, empty when they didn't.
[[nodiscard]] QString modelChosen(const Settings &settings, Capability capability);
void chooseModel(Settings &settings, Capability capability, const QString &model);
// The prebuilt voices it reads aloud with.
[[nodiscard]] QStringList voices();

// A request for this model's method (":generateContent", ":streamGenerateContent?alt=sse", ...).
[[nodiscard]] QNetworkRequest request(const Api &api, const QString &model, QStringView method, std::chrono::milliseconds timeout);

// What is sent: an answer's conversation, a picture of handwriting with what
// to do with it, a recording, a sentence to be read, texts to be embedded.
[[nodiscard]] QJsonObject answerBody(const AiRequest &request);
[[nodiscard]] QJsonObject pictureBody(const QString &prompt, const QByteArray &png);
[[nodiscard]] QJsonObject recordingBody(const QString &model, const QByteArray &audio, const QString &mimeType, const QString &language);
[[nodiscard]] QJsonObject speechBody(const QString &sentence, const QString &voice);
enum class Use : std::uint8_t { Document, Question };
[[nodiscard]] QJsonObject embedBody(const QString &model, const QStringList &texts, Use use);

// What is answered: the text of a response (or of one streamed piece), its
// thinking left out; whether it was declined; a transcript; a WAV of speech;
// the vectors of embeddings, in their order.
[[nodiscard]] QString textOf(const QJsonObject &response);
[[nodiscard]] bool declined(const QJsonObject &response);
[[nodiscard]] Recognition::Result transcriptOf(const QJsonObject &response);
[[nodiscard]] QByteArray soundOf(const QJsonObject &response);
[[nodiscard]] std::vector<std::vector<float>> vectorsOf(const QJsonObject &response);

// Whether an error says the key was refused: Gemini answers a key it doesn't
// know with 400 and API_KEY_INVALID, one without the right with 401 or 403.
[[nodiscard]] bool keyRefused(int status, const QJsonObject &body);
// Why a request failed, as the other providers say it (AiFailures).
[[nodiscard]] Failure failureOf(const Api &api, int status, const QNetworkReply &reply, const QJsonObject &body, const QString &model);

// The embeddings of the texts, waiting at most `timeout`: for a thread of its
// own or the command line. Each vector as Gemini gives it.
[[nodiscard]] std::expected<std::vector<std::vector<float>>, Failure> embed(const Api &api, const QString &model, const QStringList &texts,
                                                                            Use use, std::chrono::milliseconds timeout);

} // namespace Gemini
