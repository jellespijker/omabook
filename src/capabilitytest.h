#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <chrono>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "aicapability.h"
#include "failure.h"
#include "inkstroke.h"

class Settings;

// The Test of a row of the AI page (#170): one small real request through
// exactly what the row uses, its provider, server, key and model, as the
// registry gives them for the person's own use (ADR 0008): answers and quick
// work are asked for a two-word reply, handwriting is shown a tiny drawn
// word, speech to text hears a short bundled clip, text to speech says a
// sentence, and embeddings make one vector. What came back is kept with how
// long it took, or why it failed as a code the row words: no route, a refused
// key, no such model, no answer in time, hosted not allowed, or the AI off.
//
// Nothing is sent with the AI Off, and nothing to someone else's computer
// that the row doesn't allow, unless the person said yes to this one test
// (`hostedOnce`): it then goes through a copy of the settings that allows
// hosted for that capability alone, and theirs stay as they were.
class CapabilityTest : public QObject {
    Q_OBJECT

public:
    explicit CapabilityTest(Settings *settings, QString defaultsFile = {}, QObject *parent = nullptr);
    ~CapabilityTest() override;
    CapabilityTest(const CapabilityTest &) = delete;
    CapabilityTest &operator=(const CapabilityTest &) = delete;
    CapabilityTest(CapabilityTest &&) = delete;
    CapabilityTest &operator=(CapabilityTest &&) = delete;

    // Tests the capability; one under way for it is stopped first. With
    // `fallback`, its fallback (#174), as though the first were away: hosted
    // only by the fallback's own allowance, or `hostedOnce`.
    void run(Capability capability, bool hostedOnce, bool fallback = false);
    // Stops every test under way; what they came to is forgotten.
    void stop();
    // {state: "running", "passed" or "failed", said (the reply, the words
    // read or heard, the sentence said, the vector's length), ms, why,
    // message}; empty when none ran. Its fallback's apart.
    [[nodiscard]] QVariantMap outcome(Capability capability, bool fallback = false) const;
    // How long a test may take before it is "timeout": a minute, as a vision
    // model may first have to be loaded.
    void setTimeout(std::chrono::milliseconds timeout) noexcept { m_timeout = timeout; }

    // The code a failure is worded under on the row: "no-route",
    // "refused-key", "no-model", "timeout", "hosted-not-allowed", "ai-off",
    // else "failed".
    [[nodiscard]] static QString whyOf(Failure::Code code);
    [[nodiscard]] static QString whyOf(CapabilityLack lack);
    // The word handwriting is tested with, drawn as a pen would: "OK".
    [[nodiscard]] static std::vector<Ink::Stroke> drawnWord();

signals:
    void changed();

private:
    struct Running;
    // The capability, and whether it is its fallback that is tested.
    using Which = std::pair<Capability, bool>;
    void finish(Which which, int generation, QVariantMap outcome);

    Settings *m_settings;
    QString m_defaultsFile;
    std::chrono::milliseconds m_timeout{std::chrono::minutes(1)};
    int m_generation = 0;
    std::map<Which, std::unique_ptr<Running>> m_running;
    std::map<Which, QVariantMap> m_outcomes;
};
