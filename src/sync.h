#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <functional>

#include "library.h"

// Keeping the libraries of several computers in step.
//
// Each computer has its own whole library and its own database, which only
// its own Omabook ever writes. A sync asks the other computer's Omabook what
// it has (over ssh, which is all Tailscale needs to add), copies across the
// books each side lacks, and carries the newer reading place either way. So
// a laptop reads on the train with no network, nothing is ever opened over
// a network filesystem (where two writers can corrupt a SQLite file), and a
// dropped connection leaves both libraries whole.
//
// A book is the same book on two computers when they hold a file with the
// same content. A book removed on one computer is not fetched back to it,
// and is left alone on the other.

// Another computer's Omabook, reached by running commands there.
class Remote {
public:
    // `target` is "computer" or "computer:library folder"; `ssh` is the
    // program and its first arguments, which tests replace with a stand-in.
    explicit Remote(const QString &target, const QStringList &ssh = {});

    QString host() const { return m_host; }

    struct Result {
        int code = -1;
        QByteArray output;
        QString error;      // what the command wrote to standard error
        bool ok() const { return code == 0; }
    };
    // Runs `omabook <arguments>` there, on its library.
    Result omabook(const QStringList &arguments) const;
    // Runs a shell command there; a file here can be its input or take its output.
    Result run(const QString &command, const QString &inputFile = {}, const QString &outputFile = {}) const;
    bool fetch(const QString &remoteFile, const QString &localFile) const;
    bool send(const QString &localFile, const QString &remoteFile) const;

    // One argument, quoted for the shell on the other side.
    static QString quote(const QString &argument);

private:
    QString m_host, m_folder;
    QStringList m_ssh;
};

// Everything a library holds, as the other side of a sync needs to see it.
QJsonObject libraryManifest(const Library &library);

struct SyncSummary {
    int received = 0;           // books that came from the other computer
    int sent = 0;               // books that went to it
    int placesTaken = 0;        // reading places that were newer there
    int placesSent = 0;         // reading places that were newer here
    int collectionsTaken = 0;   // collections that changed there last, brought here
    int collectionsSent = 0;    // collections that changed here last, brought there
    QStringList problems;       // books that could not be carried, and why
    QString error;              // why the sync could not happen at all
};

SyncSummary syncLibrary(Library &library, const Remote &remote,
                        const std::function<void(const QString &title)> &progress = {});
