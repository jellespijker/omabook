#pragma once

// A stand-in for GitHub, for the tests of plugins that come from a
// repository: its API and its downloads, as far as Omabook asks them, on
// this computer, and the tarballs it serves. Nothing here reaches the network.

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>
#include <algorithm>
#include <archive.h>
#include <archive_entry.h>

struct TarEntry {
    QString name;
    QByteArray data;
    QString link; // where it points, for an entry that is a link
};

// A .tar.gz as GitHub makes one of a repository, entry for entry as given.
inline QByteArray tarball(const QList<TarEntry> &entries) {
    QTemporaryDir dir;
    const QString path = dir.filePath("repo.tar.gz");
    archive *writer = archive_write_new();
    archive_write_add_filter_gzip(writer);
    archive_write_set_format_pax_restricted(writer);
    archive_write_open_filename(writer, QFile::encodeName(path).constData());
    for (const TarEntry &given : entries) {
        archive_entry *entry = archive_entry_new();
        archive_entry_set_pathname_utf8(entry, given.name.toUtf8().constData());
        archive_entry_set_perm(entry, 0644);
        if (given.link.isEmpty()) {
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_size(entry, given.data.size());
        } else {
            archive_entry_set_filetype(entry, AE_IFLNK);
            archive_entry_set_symlink_utf8(entry, given.link.toUtf8().constData());
            archive_entry_set_size(entry, 0);
        }
        archive_write_header(writer, entry);
        if (given.link.isEmpty())
            archive_write_data(writer, given.data.constData(), given.data.size());
        archive_entry_free(entry);
    }
    archive_write_close(writer);
    archive_write_free(writer);
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// A repository holding a script plugin, as its tarball: one folder named
// after the commit, with the plugin at its root or in `subfolder`.
struct Made {
    QString version = "1.0";
    QStringList permissions{"files"};
    QString title = "Storyteller";
    QByteArray script = "omabook.commands.register({id: \"tell\", title: \"Tell a story\", place: \"library\", run: function () {}});\n"
                        "omabook.on(\"book.closed\", function (event) {});\n";
    QString subfolder;
    QList<TarEntry> more;

    QByteArray packed(const QString &top = "someone-omabook-storyteller-abc1234") const {
        const QString at = top + "/" + (subfolder.isEmpty() ? QString() : subfolder + "/");
        const QByteArray manifest = QJsonDocument(QJsonObject{{"name", title}, {"version", version}, {"author", "Someone"},
                                                              {"description", "Writes a book for you."}, {"script", "main.js"},
                                                              {"permissions", QJsonArray::fromStringList(permissions)}}).toJson();
        QList<TarEntry> entries{{top + "/README.md", "# A plugin\n", {}}, {top + "/LICENSE", "MIT\n", {}},
                                {at + "plugin.json", manifest, {}}, {at + "main.js", script, {}}, {at + "lib/words.js", "var words = 1;\n", {}}};
        return tarball(entries + more);
    }
};

inline QString sha(char letter) { return QString(40, QChar(letter)); }

// GitHub's API and its downloads, as far as Omabook asks them, on this
// computer: repositories with branches, tags, releases and a tarball per
// commit. It keeps every path it was asked, so a test can see what a check cost.
class FakeGitHub : public QTcpServer {
public:
    struct Repo {
        QString defaultBranch = "main";
        QString release;                     // the tag of the latest release, or empty
        QHash<QString, QString> refs;        // a branch or a tag, and the commit it stands at
        QStringList branches;                // which of those are branches
        QHash<QString, QByteArray> tarballs; // by commit
        bool gone = false, moved = false;
    };
    QHash<QString, Repo> repos;
    QStringList asked;
    QList<QByteArray> heads;
    bool limited = false;
    qint64 reset = 0;
    QString sendDownloadsTo; // an address to send downloads on to, instead of its own
    int downloads = 0;
    // Finding plugins: what the search by topic finds (GitHub's repository
    // objects, in the order it ranks them), how many a page holds, single
    // files of repositories (by "owner/repo" and path, as the contents API
    // serves them), and files of raw.githubusercontent.com (by their path).
    QList<QJsonObject> topical;
    int pageSize = 30;
    QHash<QString, QHash<QString, QByteArray>> files;
    QHash<QString, QByteArray> raw;
    // Gists, as the API keeps them, by id: what `gh api` sends (the fake gh
    // in fakegh.cpp) makes and changes them, and anyone may read them.
    struct Gist {
        QString owner = "ada";
        QString description;
        bool open = false;
        QHash<QString, QByteArray> files;
        bool truncated = false; // its files are sent only in part, with where the whole is
    };
    QHash<QString, Gist> gists;
    int gistsMade = 0;
    // A repository as the search answers it.
    static QJsonObject found(const QString &repository, int stars = 0, bool archived = false, const QString &description = {}) {
        return {{"full_name", repository}, {"description", description}, {"stargazers_count", stars}, {"archived", archived},
                {"default_branch", "main"}, {"pushed_at", "2026-10-01T12:00:00Z"}, {"license", QJsonObject{{"spdx_id", "MIT"}}},
                {"owner", QJsonObject{{"login", repository.section('/', 0, 0)}}}, {"topics", QJsonArray{"omabook-plugin"}}};
    }

    FakeGitHub() {
        listen(QHostAddress::LocalHost);
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    const QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", request);
                    if (!request.contains("\r\n\r\n"))
                        return;
                    const QByteArray head = request.left(request.indexOf("\r\n\r\n"));
                    // What a request sends after its head, all of it.
                    const QByteArray body = request.mid(head.size() + 4);
                    if (body.size() < header(head, "content-length").toLongLong())
                        return;
                    const QList<QByteArray> line = head.left(head.indexOf("\r\n")).split(' ');
                    route(socket, QString::fromLatin1(line.value(1)), head, QString::fromLatin1(line.value(0)), body);
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    QString api() const { return QString("http://127.0.0.1:%1").arg(serverPort()); }
    // A commit on a branch, and what the repository is at that commit.
    void push(const QString &repository, const QString &branch, const QString &commit, const QByteArray &packed) {
        Repo &repo = repos[repository];
        repo.refs.insert(branch, commit);
        if (!repo.branches.contains(branch))
            repo.branches << branch;
        repo.tarballs.insert(commit, packed);
    }
    void tag(const QString &repository, const QString &name, const QString &commit, const QByteArray &packed, bool released) {
        Repo &repo = repos[repository];
        repo.refs.insert(name, commit);
        repo.tarballs.insert(commit, packed);
        if (released)
            repo.release = name;
    }
    int count(const QString &containing) const {
        int count = 0;
        for (const QString &path : asked)
            count += path.contains(containing);
        return count;
    }
    static QByteArray header(const QByteArray &head, const QByteArray &name) {
        for (const QByteArray &line : head.split('\n'))
            if (line.toLower().startsWith(name.toLower() + ":"))
                return line.mid(name.size() + 1).trimmed();
        return {};
    }

private:
    void send(QTcpSocket *socket, int status, const QByteArray &body, const QByteArray &more = {}) {
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Answer\r\nContent-Length: " + QByteArray::number(body.size())
                      + "\r\nConnection: close\r\n" + more + "\r\n" + body);
        socket->disconnectFromHost();
    }
    // Answers with what it is asked for, unless the asker already has it.
    void sendTagged(QTcpSocket *socket, const QByteArray &head, const QByteArray &etag, const QByteArray &body) {
        if (header(head, "if-none-match") == etag)
            send(socket, 304, {}, "ETag: " + etag + "\r\n");
        else
            send(socket, 200, body, "ETag: " + etag + "\r\n");
    }

    QJsonObject gistJson(const QString &id) const {
        const Gist gist = gists.value(id);
        QJsonObject files;
        for (const auto &[name, content] : gist.files.asKeyValueRange())
            files.insert(name, QJsonObject{{"filename", name},
                                           {"content", QString::fromUtf8(gist.truncated ? content.left(10) : content)},
                                           {"truncated", gist.truncated},
                                           {"raw_url", api() + "/gistraw/" + id + "/" + name}});
        return {{"id", id},
                {"html_url", "https://gist.github.com/" + gist.owner + "/" + id},
                {"public", gist.open},
                {"description", gist.description},
                {"files", files}};
    }

    // GET, POST and PATCH of /gists[/<id>], as GitHub answers them.
    void answerGist(QTcpSocket *socket, const QString &method, const QString &id, const QByteArray &body) {
        const QJsonObject sent = QJsonDocument::fromJson(body).object();
        if (method == "POST" && id.isEmpty()) {
            const QString made = QString("%1").arg(++gistsMade, 32, 10, QChar('0'));
            gists.insert(made, Gist{.owner = "ada",
                                    .description = sent.value("description").toString(),
                                    .open = sent.value("public").toBool(false),
                                    .files = {},
                                    .truncated = false});
            answerGist(socket, "PATCH", made, body);
            return;
        }
        if (!gists.contains(id)) {
            send(socket, 404, "{\"message\": \"Not Found\"}");
            return;
        }
        if (method == "PATCH") {
            Gist &gist = gists[id];
            const QJsonObject files = sent.value("files").toObject();
            for (auto file = files.begin(); file != files.end(); ++file)
                if (file.value().isNull())
                    gist.files.remove(file.key());
                else
                    gist.files.insert(file.key(), file.value().toObject().value("content").toString().toUtf8());
            if (sent.contains("description"))
                gist.description = sent.value("description").toString();
        }
        send(socket, 200, QJsonDocument(gistJson(id)).toJson());
    }

    // Gists first, then everything else GitHub answers.
    void route(QTcpSocket *socket, const QString &whole, const QByteArray &head, const QString &method, const QByteArray &body) {
        const QStringList parts = QUrl(whole).path(QUrl::FullyEncoded).split('/', Qt::SkipEmptyParts);
        if (parts.value(0) == "gistraw") {
            asked << whole;
            heads << head;
            const QByteArray content = gists.value(parts.value(1)).files.value(parts.value(2));
            send(socket, content.isNull() ? 404 : 200, content);
        } else if (parts.value(0) == "gists" && !limited) {
            asked << whole;
            heads << head;
            answerGist(socket, method, parts.value(1), body);
        } else {
            answer(socket, whole, head);
        }
    }

    void answer(QTcpSocket *socket, const QString &whole, const QByteArray &head) {
        asked << whole;
        heads << head;
        const QUrl url(whole);
        const QString path = url.path(QUrl::FullyEncoded);
        const QStringList parts = path.split('/', Qt::SkipEmptyParts);
        // raw.githubusercontent.com, which counts nothing against a limit.
        if (parts.value(0) == "raw") {
            const QString file = parts.mid(1).join('/');
            if (!raw.contains(file))
                send(socket, 404, "404: Not Found");
            else
                sendTagged(socket, head, "\"" + QByteArray::number(qHash(raw.value(file))) + "\"", raw.value(file));
            return;
        }
        if (parts.value(0) == "dl") {
            ++downloads;
            const QByteArray packed = repos.value(parts.value(1) + "/" + parts.value(2)).tarballs.value(parts.value(3));
            send(socket, packed.isEmpty() ? 404 : 200, packed);
            return;
        }
        if (limited) {
            send(socket, 403, "{\"message\": \"API rate limit exceeded\"}",
                 "x-ratelimit-remaining: 0\r\nx-ratelimit-reset: " + QByteArray::number(reset) + "\r\n");
            return;
        }
        if (parts.value(0) == "search" && parts.value(1) == "repositories") {
            const QUrlQuery query(url);
            const QStringList words = query.queryItemValue("q", QUrl::FullyDecoded).split(QRegularExpression("[ +]"), Qt::SkipEmptyParts).mid(1);
            QJsonArray matching;
            for (const QJsonObject &repository : std::as_const(topical)) {
                const QString all = repository.value("full_name").toString() + " " + repository.value("description").toString();
                if (std::ranges::all_of(words, [&all](const QString &word) { return all.contains(word, Qt::CaseInsensitive); }))
                    matching << repository;
            }
            const int page = qMax(1, query.queryItemValue("page").toInt());
            QJsonArray items;
            for (int i = (page - 1) * pageSize; i < qMin(int(matching.size()), page * pageSize); ++i)
                items << matching.at(i);
            send(socket, 200, QJsonDocument(QJsonObject{{"total_count", matching.size()}, {"items", items}}).toJson());
            return;
        }
        const QString repository = parts.value(1) + "/" + parts.value(2);
        if (parts.value(0) == "repos" && parts.value(3) == "contents") {
            const QString file = parts.mid(4).join('/');
            if (files.value(repository).contains(file))
                send(socket, 200, files.value(repository).value(file));
            else
                send(socket, 404, "{\"message\": \"Not Found\"}");
            return;
        }
        const Repo repo = repos.value(repository);
        if (parts.value(0) != "repos" || !repos.contains(repository) || repo.gone) {
            send(socket, 404, "{\"message\": \"Not Found\"}");
            return;
        }
        if (repo.moved) {
            send(socket, 301, {}, "Location: /repositories/42/" + parts.mid(3).join('/').toLatin1() + "\r\n");
            return;
        }
        const QString what = parts.value(3), ref = QUrl::fromPercentEncoding(parts.mid(4).join('/').toLatin1());
        if (what.isEmpty()) {
            send(socket, 200, QJsonDocument(QJsonObject{{"full_name", repository}, {"default_branch", repo.defaultBranch}}).toJson());
        } else if (what == "releases") {
            if (repo.release.isEmpty())
                send(socket, 404, "{\"message\": \"Not Found\"}");
            else
                sendTagged(socket, head, "\"release-" + repo.release.toLatin1() + "\"",
                           QJsonDocument(QJsonObject{{"tag_name", repo.release}}).toJson());
        } else if (what == "commits") {
            const QString commit = repo.refs.value(ref, repo.tarballs.contains(ref) ? ref : QString());
            if (commit.isEmpty())
                send(socket, 404, "{\"message\": \"No commit found\"}");
            else
                sendTagged(socket, head, "\"" + commit.toLatin1() + "\"", commit.toLatin1());
        } else if (what == "branches") {
            if (repo.branches.contains(ref))
                send(socket, 200, QJsonDocument(QJsonObject{{"name", ref}}).toJson());
            else
                send(socket, 404, "{\"message\": \"Branch not found\"}");
        } else if (what == "tarball") {
            const QString commit = repo.refs.value(ref, ref);
            const QString base = sendDownloadsTo.isEmpty() ? api() : sendDownloadsTo;
            send(socket, 302, {}, "Location: " + (base + "/dl/" + repository + "/" + commit).toLatin1() + "\r\n");
        } else {
            send(socket, 404, "{\"message\": \"Not Found\"}");
        }
    }
};
