#include "aireach.h"

#include <QJsonArray>
#include <QStringList>

#include "ai.h"
#include "aiendpoint.h"
#include "recognition.h"
#include "settings.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QStringList ownServers(const Settings &settings) {
    QStringList own;
    for (const auto &address : settings.recognition().value("own"_L1).toArray())
        own << address.toString();
    return own;
}

// The model a request of `word` goes to, of the models the settings keep for the server.
[[nodiscard]] QString modelOf(const AiModels &models, const QString &word) {
    if (word.isEmpty() || word == "main"_L1)
        return models.main;
    return word == "quick"_L1 ? models.quick : word;
}

} // namespace

bool AiReach::hosted(const Settings &settings, const QString &model) {
    const AiChoice choice = Ai::choose(settings);
    if (choice.id != "openai"_L1)
        return true;
    if (AiEndpoints::runsElsewhere(modelOf(Ai::models(settings, choice.modelsKey()), model.trimmed())))
        return true;
    // Omarchy's local models are found by asking Omarchy, on this computer.
    if (AiEndpoints::preset(settings) == "omarchy-local"_L1)
        return false;
    return Recognition::reach(AiEndpoints::baseUrl(settings), ownServers(settings)) == Recognition::Reach::Hosted;
}
