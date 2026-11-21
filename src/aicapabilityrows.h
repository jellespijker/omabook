#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include "aicapability.h"

class Settings;

// The AI page of Settings (#144, ADR 0008): a row per capability, as the
// window shows it. Each row says the registry's status (whether it may be
// used, the provider and model, whether that is someone else's computer, and
// why not, as a code the window words), what the person chose (whether it may
// be hosted, which of the providers that serve it, the model, a server's
// address and whether a key is kept for it), and the providers it may choose
// from, by their ids. The window words those too; a provider that comes
// (Gemini, #145; more voices, #147) is an id more in `offered`.
//
// Each provider stays set up where it always was (`ai_provider`,
// `ai_endpoint`, `recognition`, `meaning`, `read_aloud`), which an older
// Omabook on the same config.json reads; `capabilities` keeps each one's
// hosted choice and, where there is a choice of provider, which.
class AiCapabilityRows : public QObject {
    Q_OBJECT
    // One map per capability, in the order of Capabilities::all:
    //   capability, available, hosted, provider, model, why, message,
    //   hostedAllowed, chosen (the provider's id), offered (ids),
    //   address, addressDefault, hasKey, inTheClear, modelChoice, modelDefault,
    //   and for speech to text, timed (whether its words have their moments)
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)
    // Off stops every model (#133): `ai_provider` "off".
    Q_PROPERTY(bool aiOn READ aiOn NOTIFY changed)

public:
    explicit AiCapabilityRows(Settings *settings, QString defaultsFile = {}, QObject *parent = nullptr);

    [[nodiscard]] QVariantList rows() const { return m_rows; }
    [[nodiscard]] bool aiOn() const;
    [[nodiscard]] QVariantMap row(Capability capability) const;

    // Each takes a capability by its name ("answers", "speech-to-text", ...);
    // one that isn't, or a choice it can't take, changes nothing.
    Q_INVOKABLE void allowHosted(const QString &capability, bool allowed);
    Q_INVOKABLE void choose(const QString &capability, const QString &provider);
    Q_INVOKABLE void setModel(const QString &capability, const QString &model);
    // The address of the server it is sent to; "" for the AI server.
    Q_INVOKABLE void setAddress(const QString &capability, const QString &address);
    // Keeps the key for that server in the keyring; "" forgets it. Says why
    // when it couldn't be kept, else "".
    Q_INVOKABLE QString setKey(const QString &capability, const QString &key);
    Q_INVOKABLE void setAiOn(bool on);
    // Whether the page is in front of somebody: the rows are worked out, and
    // kept up with the settings, only then, as they ask the keyring and PATH.
    Q_INVOKABLE void watch(bool on);
    // Asks again what is installed and running, which nothing says changed.
    Q_INVOKABLE void refresh();

signals:
    void changed();
    // A key was kept or forgotten in the keyring, which the settings don't say.
    void keyKept();

private:
    // Writes `capabilities` from the older keys where it isn't there yet.
    void carryOver();
    // Puts one field of the provider's set-up (its model, its address) in the older key it is kept in.
    void setUp(Capability capability, const QString &field, const QString &value);

    Settings *m_settings;
    QString m_defaultsFile;
    QVariantList m_rows;
    bool m_watching = false;
};
