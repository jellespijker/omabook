#include "aiendpoint.h"

#include <QFile>
#include <QHostAddress>
#include <QJsonObject>
#include <QUrl>
#include <algorithm>
#include <array>

#include "omarchylocal.h"
#include "secretstore.h"
#include "settings.h"

namespace {

using namespace Qt::StringLiterals;

constexpr std::array endpoints{
    // Its address and key come from Omarchy, for the model that is running.
    AiEndpoint{"omarchy-local", "Omarchy's local model", "", "", false, false},
    // Ollama's OpenAI-compatible API, not its own: the one every server has.
    AiEndpoint{"ollama", "Ollama", "http://localhost:11434/v1", "", false, false},
    AiEndpoint{"openai", "OpenAI", "https://api.openai.com/v1", "OPENAI_API_KEY", true, true},
    // OpenRouter, Groq, Mistral, DeepSeek, xAI, Gemini, LM Studio, llama.cpp,
    // vLLM: any address, with a key if it takes one.
    AiEndpoint{"custom", "A server", "", "", false, true},
};

QString trimmedUrl(QString url) {
    url = url.trimmed();
    while (url.endsWith('/'))
        url.chop(1);
    return url;
}

// The host and port of an address, as a failure names a server.
QString hostOf(const QString &baseUrl) {
    const QUrl url(baseUrl);
    return url.port() < 0 ? url.host() : u"%1:%2"_s.arg(url.host()).arg(url.port());
}

QString secretName(const QString &baseUrl) { return u"ai-server:"_s + trimmedUrl(baseUrl); }

// The key from wherever this row takes it: its variable wins over the keyring.
QString keyFor(const AiEndpoint &endpoint, const QString &baseUrl) {
    if (const QString variable = QString::fromLatin1(endpoint.keyVariable); !variable.isEmpty())
        if (const QString key = qEnvironmentVariable(qPrintable(variable)).trimmed(); !key.isEmpty())
            return key;
    return AiEndpoints::keptKey(baseUrl, Secrets::shared());
}

} // namespace

QString AiServer::currentKey() const {
    if (keyFile.isEmpty())
        return key;
    QFile file(keyFile);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
}

QString AiServer::host() const { return hostOf(baseUrl); }

std::span<const AiEndpoint> AiEndpoints::all() { return endpoints; }

const AiEndpoint *AiEndpoints::find(const QString &id) {
    const auto *const row =
        std::ranges::find_if(endpoints, [&id](const AiEndpoint &endpoint) { return id == QLatin1StringView(endpoint.id); });
    return row == endpoints.end() ? nullptr : &*row;
}

QString AiEndpoints::preset(const Settings &settings) { return settings.aiEndpoint().value("preset"_L1).toString(u"ollama"_s); }

QString AiEndpoints::baseUrl(const Settings &settings) {
    QString typed = trimmedUrl(settings.aiEndpoint().value("base_url"_L1).toString());
    if (!typed.isEmpty())
        return typed;
    const AiEndpoint *row = find(preset(settings));
    return row ? QString::fromLatin1(row->baseUrl) : QString();
}

QString AiEndpoints::name(const Settings &settings) {
    const QString named = preset(settings);
    const AiEndpoint *row = find(named);
    if (named == "custom"_L1 || !row)
        return hostOf(baseUrl(settings));
    return QString::fromLatin1(row->name);
}

QString AiEndpoints::modelsKey(const Settings &settings) {
    // A local model's address changes with the model, so they go by the preset.
    const QString named = preset(settings);
    return u"openai:"_s + (named == "omarchy-local"_L1 ? named : baseUrl(settings));
}

QString AiEndpoints::missing(const Settings &settings) {
    const QString named = preset(settings);
    const AiEndpoint *row = find(named);
    if (!row)
        return u"Omabook doesn't know an AI server called \"%1\"; see the settings."_s.arg(named);
    if (named == "omarchy-local"_L1)
        return OmarchyLocal::installed() ? QString()
                                         : u"Omarchy's local models aren't installed here (there is no omarchy-agent-local command)."_s;
    if (baseUrl(settings).isEmpty())
        return u"No address is set for the AI server; type one in the settings."_s;
    if (row->keyRequired && keyFor(*row, baseUrl(settings)).isEmpty())
        return u"Set %1, or put a key in the settings, to use %2."_s.arg(QString::fromLatin1(row->keyVariable),
                                                                         QString::fromLatin1(row->name));
    return {};
}

std::expected<AiServer, QString> AiEndpoints::server(const Settings &settings, const QString &wanted, const QString &omarchyProgram) {
    const QString named = preset(settings);
    if (named == "omarchy-local"_L1) {
        const auto models = OmarchyLocal::running(omarchyProgram);
        if (!models)
            return std::unexpected(models.error());
        const auto model = OmarchyLocal::pick(*models, wanted);
        if (!model)
            return std::unexpected(model.error());
        const QString name = model->host.isEmpty() ? u"Omarchy's local model"_s : u"Omarchy's model on "_s + model->host;
        return AiServer{.preset = named,
                        .name = name,
                        .baseUrl = trimmedUrl(model->baseUrl),
                        .key = {},
                        .keyFile = model->keyFile,
                        .model = model->model};
    }
    if (const QString problem = missing(settings); !problem.isEmpty())
        return std::unexpected(problem);
    const AiEndpoint &row = *find(named);
    const QString url = baseUrl(settings);
    return AiServer{.preset = named, .name = name(settings), .baseUrl = url, .key = keyFor(row, url), .keyFile = {}, .model = {}};
}

QString AiEndpoints::keptKey(const QString &baseUrl, const SecretStore &store) { return store.get(secretName(baseUrl)); }

std::expected<void, QString> AiEndpoints::keepKey(const QString &baseUrl, const QString &key, SecretStore &store) {
    if (key.trimmed().isEmpty()) {
        if (!store.forget(secretName(baseUrl)))
            return std::unexpected(u"The key can't be taken out of the keyring."_s);
        return {};
    }
    QString error;
    if (!store.put(secretName(baseUrl), key.trimmed(), &error))
        return std::unexpected(u"The key couldn't be kept: "_s + error);
    return {};
}

bool AiEndpoints::hasKey(const Settings &settings) {
    const QString named = preset(settings);
    if (named == "omarchy-local"_L1)
        return true; // Omarchy keeps it, and hands it over with every request.
    const AiEndpoint *row = find(named);
    return row && !keyFor(*row, baseUrl(settings)).isEmpty();
}

bool AiEndpoints::inTheClear(const QString &baseUrl) {
    const QUrl url(baseUrl.trimmed());
    if (url.scheme() != "http"_L1)
        return false;
    const QString host = url.host().toLower();
    if (host == "localhost"_L1 || host.endsWith(".ts.net"_L1))
        return false;
    QHostAddress address;
    if (!address.setAddress(host))
        return host.contains('.'); // A name of one word is the tailnet's own (MagicDNS).
    // Tailscale's addresses, in IPv4 and IPv6.
    return !address.isLoopback() && !address.isInSubnet(QHostAddress::parseSubnet(u"100.64.0.0/10"_s))
           && !address.isInSubnet(QHostAddress::parseSubnet(u"fd7a:115c:a1e0::/48"_s));
}

bool AiEndpoints::runsElsewhere(const QString &model) { return model.endsWith(":cloud"_L1) || model.endsWith("-cloud"_L1); }
