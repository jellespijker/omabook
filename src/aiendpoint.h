#pragma once

#include <QString>
#include <expected>
#include <span>

#include "aicapability.h"

class SecretStore;
class Settings;

// A kind of server that speaks the OpenAI-compatible API, as a row of data:
// what makes one differ from another is its address and where its key comes
// from, never how it talks. A vendor becomes a row only when it needs
// something its address can't say; every other one is "custom" with its URL.
struct AiEndpoint {
    const char *id;      // "omarchy-local", "ollama", "openai", "custom": what `ai_endpoint` names as its preset
    const char *name;    // what to call it
    const char *baseUrl; // the address it has unless another is typed in; empty when there is none to assume
    // Where its key comes from besides the keyring: this environment
    // variable, which then wins; empty for none.
    const char *keyVariable;
    bool keyRequired;   // whether it can't answer without one
    bool leavesMachine; // whether what is asked goes to someone else's computer
};

// One server to ask, made from the settings: the row it is, where it is,
// and how its key is read when a request is sent.
struct AiServer {
    QString preset;
    QString name;    // "Ollama", "OpenAI", "Omarchy's local model", or the host of a custom one
    QString baseUrl; // ".../v1", without the slash at the end
    QString key;     // empty for none
    // A file the key is read from for each request, over `key`: Omarchy's
    // local models, whose key may change while Omabook runs.
    QString keyFile;
    // A model the server is known to serve, used when none is chosen.
    QString model;

    // The key for the request about to be sent.
    [[nodiscard]] QString currentKey() const;
    // The host and port, as a failure names the server.
    [[nodiscard]] QString host() const;
};

namespace AiEndpoints {

// The built-in rows, Omarchy's local models first.
[[nodiscard]] std::span<const AiEndpoint> all();
// The row of that id, or null.
[[nodiscard]] const AiEndpoint *find(const QString &id);

// The preset and address the settings hold (`ai_endpoint`), with the
// preset's own address where none was typed in.
[[nodiscard]] QString preset(const Settings &settings);
[[nodiscard]] QString baseUrl(const Settings &settings);
// What to call the server the settings name: the preset's name, or the
// host of a custom one.
[[nodiscard]] QString name(const Settings &settings);
// What `ai_models` keeps this server's two models under: they are its own,
// and another server's names would mean nothing to it.
[[nodiscard]] QString modelsKey(const Settings &settings);

// The server the settings name, with its key, without asking anything of
// the network. Omarchy's local models are found by asking Omarchy (see
// OmarchyLocal), which the command line waits for and a test stands in for:
// `omarchyProgram` is its command, empty for the one on PATH, and `wanted`
// the model chosen, whose server is the one to ask. Fails, in English, when
// it isn't set up: no address, a key that is needed and missing, no local
// model running.
[[nodiscard]] std::expected<AiServer, QString> server(const Settings &settings, const QString &wanted = {},
                                                      const QString &omarchyProgram = {});
// What `server` would say is missing, without asking Omarchy anything:
// what the choice of AI says before a request is made. Empty when nothing is.
[[nodiscard]] QString missing(const Settings &settings);
// The same as a code: none, no server (or no address), not installed, no key.
[[nodiscard]] CapabilityLack lacking(const Settings &settings);

// The key kept for a server at this address, in the keyring (never in
// config.json); empty for none. Putting an empty key forgets it.
[[nodiscard]] QString keptKey(const QString &baseUrl, const SecretStore &store);
[[nodiscard]] std::expected<void, QString> keepKey(const QString &baseUrl, const QString &key, SecretStore &store);
// Whether a key is set for the server the settings name, from wherever it comes.
[[nodiscard]] bool hasKey(const Settings &settings);

// Whether text sent to this address crosses a network unencrypted: http://
// to a computer that is neither this one nor on the tailnet.
[[nodiscard]] bool inTheClear(const QString &baseUrl);
// Whether a model's name says it runs somewhere else than the server asked,
// as Ollama's "...:cloud" models do: the text then leaves for that service.
[[nodiscard]] bool runsElsewhere(const QString &model);

} // namespace AiEndpoints
