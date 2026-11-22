#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>
#include <functional>
#include <map>
#include <memory>

#include "aicapability.h"

class ModelHelp;
class Settings;

// A capability's missing model, as a plugin is offered to mend it (#172):
// `ai.status`'s `remedy` (the Notebooks plugin's handwriting message offers
// what the window's AI page does), and `ai.remedy`, which does it when the
// person presses the plugin's button. One ModelHelp per capability.
class ScriptRemedy : public QObject {
    Q_OBJECT

public:
    explicit ScriptRemedy(Settings *settings, QObject *parent = nullptr);
    ~ScriptRemedy() override;
    ScriptRemedy(const ScriptRemedy &) = delete;
    ScriptRemedy &operator=(const ScriptRemedy &) = delete;
    ScriptRemedy(ScriptRemedy &&) = delete;
    ScriptRemedy &operator=(ScriptRemedy &&) = delete;

    // What can be done: {action, model, size, server}, as ModelHelp::remedy
    // says it. The server is asked the first time, so it is known when asked again.
    [[nodiscard]] QVariantMap remedy(Capability capability);

    // Chooses the model on the server that fits, or downloads the one
    // suggested and chooses it, telling how far (0 to 1); then `done` with
    // the model, or `failed` with why, in the desktop's language.
    struct Told {
        std::function<void(double)> progress;
        std::function<void(const QString &model)> done;
        std::function<void(const QString &why)> failed;
    };
    void carryOut(Capability capability, Told told);

private:
    [[nodiscard]] ModelHelp &help(Capability capability);

    QPointer<Settings> m_settings;
    std::map<Capability, std::unique_ptr<ModelHelp>> m_helps;
};
