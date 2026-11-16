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
// same content. Of a book both have, the newer details win (Book::detailsAt).
// A book removed on one computer is not fetched back to it, and is removed
// on the other too, to the trash, unless it changed there since.

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
    // Copies a file there; `error`, when given, says why it could not.
    bool send(const QString &localFile, const QString &remoteFile, QString *error = nullptr) const;

    // One argument, quoted for the shell on the other side.
    static QString quote(const QString &argument);

private:
    QString m_host, m_folder;
    QStringList m_ssh;
};

// Everything a library holds, as the other side of a sync needs to see it.
QJsonObject libraryManifest(const Library &library);

struct SyncOptions {
    // Whose details a book both sides have takes where they differ and no
    // time says which are newer (books from before the times were kept):
    // neither (they are only counted), this side's, or the other side's.
    enum class Details { Undecided, Mine, Theirs };
    Details details = Details::Undecided;
    // Changes nothing on either side, and says what a sync would do.
    bool dryRun = false;
};

struct SyncSummary {
    int received = 0;           // books that came from the other computer
    int sent = 0;               // books that went to it
    int placesTaken = 0;        // reading places that were newer there
    int placesSent = 0;         // reading places that were newer here
    int detailsTaken = 0;       // books whose details were newer there, taken here
    int detailsSent = 0;        // books whose details were newer here, given there
    int readingTaken = 0;       // books given the shelf, stars or finished day said there
    int readingSent = 0;        // books given there what is said of them here
    int removedHere = 0;        // books removed here, as they were there
    int removedThere = 0;       // books removed there, as they were here
    int detailsUndecided = 0;   // books whose details differ with no time to say which are newer
    int collectionsTaken = 0;   // collections that changed there last, brought here
    int collectionsSent = 0;    // collections that changed here last, brought there
    bool dryRun = false;        // nothing was changed; the counts are what would be
    // The titles each count is of, for showing on a terminal (never logged).
    struct Titles {
        QStringList received, sent, placesTaken, placesSent, detailsTaken, detailsSent, readingTaken, readingSent,
            removedHere, removedThere, detailsUndecided;
    } titles;
    QStringList problems;       // books that could not be carried, and why
    QString error;              // why the sync could not happen at all

    // The counts as `omabook sync --json` and the MCP server give them, with
    // the titles of a dry run, and of the books whose details are undecided.
    [[nodiscard]] QJsonObject toJson() const;
};

SyncSummary syncLibrary(Library &library, const Remote &remote,
                        const std::function<void(const QString &title)> &progress = {}, const SyncOptions &options = {});
