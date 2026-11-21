#pragma once

#include <QJsonObject>
#include <QString>
#include <optional>

#include "aicapability.h"

class Settings;

// The one privacy rule every capability is held to (ADR 0008), where there
// were three copies (recognition's, the meaning index's, reading aloud's):
//
// - A provider is the person's own (this computer, the tailnet, an address
//   they marked as theirs) or hosted; a model whose name says it runs
//   elsewhere (`...:cloud`) is hosted however near its server is.
// - A hosted provider is used only for a capability the person allowed hosted
//   for, and, when someone asks (a plugin), only when that one did not keep to
//   the person's own (`hosted: false`). Whether a plugin must ask for hosted
//   outright is the plugin API's to say (ScriptAi): for answers and quick it
//   takes the person's choice, for the other four it must ask.
namespace AiPrivacy {

// What the one asking said of hosted providers: nothing (the person's own use,
// or a plugin that takes their choice), yes, or no.
using HostedAsked = std::optional<bool>;

// Whether a server at this address, asked with this model, is someone else's computer.
[[nodiscard]] bool hosted(const Settings &settings, const QString &baseUrl, const QString &model);

// What `capabilities` is for a config.json from before it, from the older
// keys (ADR 0008): answers and quick keep their provider, with hosted allowed
// where it is hosted, since that is where the text already went; the others
// take recognition's, read aloud's and the meaning index's "hosted".
[[nodiscard]] QJsonObject carriedOver(const Settings &settings);
// Writes that into config.json, once, where there are no capabilities yet;
// on every start, so the window and the command line both carry it over. On
// the first start of all, with no config.json yet, it writes fresh() instead.
void carryOver(Settings &settings);

// The capability as the settings choose it: `capabilities` where they are
// kept, else what carriedOver() makes of the older keys.
[[nodiscard]] QJsonObject chosen(const Settings &settings, Capability capability);
[[nodiscard]] bool hostedAllowed(const Settings &settings, Capability capability);

// Why a provider, hosted or not, may not be used for this capability:
// HostedNotAllowed, HostedNotChosen, or None when it may.
[[nodiscard]] CapabilityLack refusal(const Settings &settings, Capability capability, bool hosted, HostedAsked asked);

} // namespace AiPrivacy
