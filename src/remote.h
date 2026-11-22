#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <expected>

// Another computer's Omabook, reached by running commands there over ssh
// (which is all Tailscale needs to add). What a sync says to the other side
// goes through here; what it says is in sync.h. A hub is reached through its
// session instead (HubRemote, #188), which takes the same few things a sync
// asks and runs no command: so a sync asks for a folder there with
// makeFolder(), never by a command of its own.
class Remote {
public:
    // `target` is "computer" or "computer:library folder"; `ssh` is the
    // program and its first arguments, which tests replace with a stand-in.
    explicit Remote(const QString &target, const QStringList &ssh = {});
    virtual ~Remote() = default;
    Remote(const Remote &) = default;
    Remote &operator=(const Remote &) = default;
    Remote(Remote &&) noexcept = default;
    Remote &operator=(Remote &&) noexcept = default;

    QString host() const { return m_host; }
    // As it was given: "computer" or "computer:library folder".
    [[nodiscard]] QString target() const { return m_folder.isEmpty() ? m_host : m_host + ':' + m_folder; }

    struct Result {
        int code = -1;
        QByteArray output;
        QString error; // what the command wrote to standard error
        bool ok() const { return code == 0; }
    };
    // Runs `omabook <arguments>` there, on its library; a file here can be its input.
    virtual Result omabook(const QStringList &arguments, const QString &inputFile = {}) const;
    // Runs a shell command there; a file here can be its input or take its output.
    virtual Result run(const QString &command, const QString &inputFile = {}, const QString &outputFile = {}) const;
    virtual bool fetch(const QString &remoteFile, const QString &localFile) const;
    // Copies a file there, or says why it could not.
    [[nodiscard]] virtual std::expected<void, QString> send(const QString &localFile, const QString &remoteFile) const;
    // A new empty folder there, for what is sent to it; empty when there is no room.
    [[nodiscard]] virtual QString makeFolder() const;
    // Removes such a folder, and what was sent to it.
    virtual void dropFolder(const QString &folder) const;
    // Hands the other side a document to take in: copies it into a scratch
    // folder there, runs `omabook <arguments> <the file>` on it, and removes
    // the folder. Answers how much it changed there (the command's
    // "changed"), or why it couldn't.
    std::expected<int, QString> handOver(const QJsonDocument &document, const QStringList &arguments) const;
    // The same, answering what the command printed (a JSON object).
    [[nodiscard]] std::expected<QJsonObject, QString> exchange(const QJsonDocument &document, const QStringList &arguments) const;

    // One argument, quoted for the shell on the other side.
    static QString quote(const QString &argument);

private:
    QString m_host, m_folder;
    QStringList m_ssh;
};
