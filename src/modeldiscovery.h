#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtTypes>
#include <cstdint>
#include <optional>

#include "aicapability.h"

// Models to be found beyond what a server has (#175), on Ollama's library and
// on Hugging Face, by what a capability needs and by what fits the computer
// it would run on: the knowledge behind "Find a model…", without the network
// (ModelSearch asks, from the person's press only, and sends nothing of theirs
// but the words searched for and the capability's kind).
//
// - Ollama has no documented API to search its library (its API is a server's
//   own: /api/tags, /api/pull, ...), so its search page, ollama.com/search, is
//   read; each model's size and licence come from its registry's manifest.
// - Hugging Face's Hub API (/api/models) is searched by task and by GGUF, which
//   Ollama pulls as `hf.co/<repo>:<quant>`; a repository's files give the size.
// - Speech either way isn't served by Ollama: whisper.cpp's models and Piper's
//   voices are files of their repositories on the Hub, kept where those engines
//   look for them.
namespace Discovery {

enum class Source : std::uint8_t { Ollama, HuggingFace };
// Whether it fits: in the GPU's free memory, in the computer's (slower), too
// big for either, or can't be told (a server elsewhere, whose memory isn't said).
enum class Fit : std::uint8_t { Gpu, Memory, TooBig, Unknown };

// What a model is, as found.
struct Found {
    Source source = Source::Ollama;
    // What is pulled onto Ollama and chosen ("qwen2.5vl:3b", "hf.co/<repo>:Q4_K_M"),
    // or for speech the file's name ("ggml-base.en.bin", "en_US-amy-medium").
    QString name;
    QString repository; // Hugging Face's, or Ollama's library name
    QString file;       // for speech, its path in the repository
    QString description;
    QString licence;
    qint64 bytes = 0; // its download, as its files say; 0 where not known
    qint64 needs = 0; // the memory it needs to run, worked out from that
    qint64 downloads = 0;
    qint64 likes = 0;
    bool hosted = false; // Ollama's ":cloud": it runs on Ollama's computers
    Fit fit = Fit::Unknown;

    // {source ("ollama", "hugging-face"), name, repository, page, description,
    // licence, bytes, needs, downloads, likes, hosted, fit ("gpu", "memory",
    // "too-big", "unknown")}.
    [[nodiscard]] QVariantMap toMap() const;
};

// What a capability is looked for by: the Hub's task, and Ollama's category
// (empty where it has none: answers and quick are any model that talks).
[[nodiscard]] QString hubTask(Capability capability);
[[nodiscard]] QString libraryCategory(Capability capability);
// Whether a model found is pulled onto an Ollama server (else it is a file of
// whisper.cpp or Piper on this computer).
[[nodiscard]] bool pulled(Capability capability);
// The repository of the Hub the speech files are in: whisper.cpp's, Piper's.
[[nodiscard]] QString speechRepository(Capability capability);

// The results of Ollama's search page, each size of a model one result, in
// its order; at most `most`.
[[nodiscard]] QList<Found> fromLibrary(const QString &html, qsizetype most = 12);
// The size of a model's download from its registry manifest (the layers'
// sizes), and the digest of its licence's layer, if it has one.
struct Manifest {
    qint64 bytes = 0;
    QString licence; // the digest of its licence's blob
};
[[nodiscard]] Manifest fromManifest(const QJsonObject &manifest);
// The licence's name from its text: its first line that says something.
[[nodiscard]] QString licenceName(const QString &text);

// The models the Hub's /api/models answered, with their licence from their tags.
[[nodiscard]] QList<Found> fromHub(const QJsonArray &models);
// The GGUF file Ollama would pull from a repository's files (Q4_K_M, its
// default, else the smallest): its quantisation and size.
struct Gguf {
    QString quant;
    qint64 bytes = 0;
};
[[nodiscard]] std::optional<Gguf> ggufOf(const QJsonArray &tree);
// The speech files of a repository's tree, those whose names have all the
// words searched for: whisper.cpp's ggml-*.bin, Piper's *.onnx.
[[nodiscard]] QList<Found> speechFiles(Capability capability, const QJsonArray &tree, const QString &terms, qsizetype most = 12);

// The bytes of a model of this many parameters ("3b", "0.6b", "300m") at
// Ollama's default quantisation, Q4_K_M; 0 for none.
[[nodiscard]] qint64 bytesOfParameters(const QString &size);
// The memory a model of that download needs to run: the weights, and room
// for what it is asked (a context's worth) on top.
[[nodiscard]] qint64 needs(Capability capability, qint64 bytes);

// The memory free on the computer it would run on, where it can be told.
struct Free {
    qint64 memory = 0; // the computer's, available
    qint64 gpu = 0;    // the GPU's, free
    bool known = false;
};
// This computer's: /proc/meminfo's MemAvailable, and each GPU's free VRAM
// (/sys/class/drm/card*/device/mem_info_vram_total less _used); `root` stands
// for "/" in a test.
[[nodiscard]] Free here(const QString &root = QStringLiteral("/"));
[[nodiscard]] Fit fit(qint64 needs, const Free &free);

} // namespace Discovery
