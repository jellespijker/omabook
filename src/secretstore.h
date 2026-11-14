#pragma once

#include <QHash>
#include <QString>
#include <memory>

// Where tokens and keys are kept: never in config.json, which is a file
// people read, sync and paste into bug reports. A secret goes by the name of
// what it opens ("hardcover", "openlibrary", "goodreads").
class SecretStore {
public:
    SecretStore() = default;
    virtual ~SecretStore() = default;
    SecretStore(const SecretStore &) = delete;
    SecretStore &operator=(const SecretStore &) = delete;

    // Whether what is put here is still here after a restart. When not,
    // `why` says so in English, for the window and the command line to show.
    virtual bool lasting(QString *why = nullptr) const = 0;
    // The secret, or empty when there is none.
    virtual QString get(const QString &name) const = 0;
    // False, with `error`, when it couldn't be kept at all.
    virtual bool put(const QString &name, const QString &secret, QString *error = nullptr) = 0;
    // Forgets it; true when it is gone, also when it was never there.
    virtual bool forget(const QString &name) = 0;
};

// Secrets for as long as the program runs, and no longer: what tests use,
// and what a desktop without a keyring falls back to.
class SessionSecrets : public SecretStore {
public:
    bool lasting(QString *why = nullptr) const override;
    QString get(const QString &name) const override { return m_secrets.value(name); }
    bool put(const QString &name, const QString &secret, QString *error = nullptr) override;
    bool forget(const QString &name) override;

private:
    QHash<QString, QString> m_secrets;
};

// The desktop's keyring, through the Secret Service on the session bus
// (GNOME Keyring on Omarchy; KWallet and KeePassXC speak it too). Without
// one, secrets are kept for the session only and `lasting` says why: they
// are never written to a file instead.
class KeyringSecrets : public SecretStore {
public:
    KeyringSecrets();
    ~KeyringSecrets() override;

    bool lasting(QString *why = nullptr) const override;
    QString get(const QString &name) const override;
    bool put(const QString &name, const QString &secret, QString *error = nullptr) override;
    bool forget(const QString &name) override;

private:
    struct Private;
    std::unique_ptr<Private> d;
};

namespace Secrets {

// The store everything uses unless it is handed another: the keyring.
// Tests put their own in its place and take it out again with nullptr.
SecretStore &shared();
void useInstead(SecretStore *store);

} // namespace Secrets
