#include "githublink.h"

#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTimer>
#include <chrono>
#include <optional>
#include <utility>

using namespace Qt::StringLiterals;

namespace {

// How long `gh` may take to send a gist before it is given up on.
constexpr auto ghTakes = std::chrono::minutes(2);

[[nodiscard]] bool isGistId(const QString &text) {
    static const QRegularExpression id(u"^[0-9a-f]{20,40}$"_s, QRegularExpression::CaseInsensitiveOption);
    return id.match(text).hasMatch();
}

[[nodiscard]] bool isName(const QString &text) {
    static const QRegularExpression name(u"^[A-Za-z0-9._-]+$"_s);
    return name.match(text).hasMatch() && text != "."_L1 && text != ".."_L1;
}

[[nodiscard]] std::optional<GitHubLink::Address> gistIn(const QString &host, const QStringList &parts) {
    // gist.github.com/<id>, /<owner>/<id> or /<owner>/<id>/<revision>;
    // gist.githubusercontent.com/<owner>/<id>/raw/…
    if (host == "gist.github.com"_L1)
        for (const QString &part : parts.mid(0, 2))
            if (isGistId(part))
                return GitHubLink::Address{.gist = part.toLower(), .owner = {}, .repo = {}, .ref = {}, .path = {}};
    if (host == "gist.githubusercontent.com"_L1 && parts.size() >= 2 && isGistId(parts.at(1)))
        return GitHubLink::Address{.gist = parts.at(1).toLower(), .owner = {}, .repo = {}, .ref = {}, .path = {}};
    return std::nullopt;
}

[[nodiscard]] std::optional<GitHubLink::Address> fileIn(const QString &host, const QStringList &parts) {
    qsizetype at = 0; // where the ref is
    if (host == "github.com"_L1 && parts.size() >= 5 && (parts.at(2) == "blob"_L1 || parts.at(2) == "raw"_L1))
        at = 3;
    else if (host == "raw.githubusercontent.com"_L1 && parts.size() >= 6 && parts.at(2) == "refs"_L1)
        at = 4; // …/refs/heads/<branch>/<path>
    else if (host == "raw.githubusercontent.com"_L1 && parts.size() >= 4)
        at = 2;
    if (at == 0 || !isName(parts.at(0)) || !isName(parts.at(1)))
        return std::nullopt;
    return GitHubLink::Address{
        .gist = {}, .owner = parts.at(0), .repo = parts.at(1), .ref = parts.at(at), .path = parts.mid(at + 1).join(u'/')};
}

[[nodiscard]] QString encodedPath(const QString &path) {
    QStringList parts;
    for (const QString &part : path.split(u'/', Qt::SkipEmptyParts))
        parts << QString::fromLatin1(QUrl::toPercentEncoding(part));
    return parts.join(u'/');
}

[[nodiscard]] QString where(const GitHubLink::Address &address) {
    return u"%1 in %2/%3 at %4"_s.arg(address.path, address.owner, address.repo, address.ref);
}

[[nodiscard]] std::expected<SpaceSnapshot, QString> snapshotIn(const QByteArray &bytes, const QString &place) {
    auto snapshot = SpaceSnapshotFile::read(bytes);
    if (!snapshot)
        return std::unexpected(u"%1 doesn't hold a shared collection: %2"_s.arg(place, snapshot.error()));
    return snapshot;
}

// Of a gist's files, the space's: the one ending in .omabook-space, else its only file.
[[nodiscard]] std::optional<QJsonObject> spaceFileOf(const QJsonObject &files) {
    QList<QJsonObject> spaces;
    for (auto file = files.begin(); file != files.end(); ++file)
        if (file.key().endsWith(QLatin1StringView(SpaceSnapshotFile::suffix)))
            spaces << file.value().toObject();
    if (spaces.size() == 1)
        return spaces.first();
    if (spaces.isEmpty() && files.size() == 1)
        return files.begin().value().toObject();
    return std::nullopt;
}

} // namespace

std::expected<GitHubLink::Address, QString> GitHubLink::parse(const QString &text) {
    QString rest = text.trimmed().section(u'?', 0, 0).section(u'#', 0, 0);
    if (isGistId(rest))
        return Address{.gist = rest.toLower(), .owner = {}, .repo = {}, .ref = {}, .path = {}};
    static const QRegularExpression scheme(u"^https?://"_s, QRegularExpression::CaseInsensitiveOption);
    rest.remove(scheme);
    if (rest.startsWith("www."_L1, Qt::CaseInsensitive))
        rest = rest.mid(4);
    const QString host = rest.section(u'/', 0, 0).toLower();
    QStringList parts;
    for (const QString &part : rest.section(u'/', 1).split(u'/', Qt::SkipEmptyParts))
        parts << QUrl::fromPercentEncoding(part.toUtf8());
    if (const auto gist = gistIn(host, parts))
        return *gist;
    if (const auto file = fileIn(host, parts); file && !file->path.isEmpty())
        return *file;
    if (host == "github.com"_L1 && parts.size() >= 2)
        return std::unexpected(u"%1 is a repository, not a file in it: link the shared file itself, "
                               "https://github.com/<owner>/<repo>/blob/<branch>/<file>.omabook-space."_s.arg(text.trimmed()));
    return std::unexpected(u"%1 is no gist or file of a GitHub repository. Give a gist's link or id, or a file's link: "
                           "https://github.com/<owner>/<repo>/blob/<branch>/<file>, or its raw link."_s.arg(text.trimmed()));
}

GitHubLink::GitHubLink(QString gh, const PluginSources::Service &service)
    : m_gh(std::move(gh)), m_service(service), m_sources(std::make_unique<PluginSources>(PluginTools::Context{})) {
    m_sources->setService(service);
}

GitHubLink::~GitHubLink() = default;

std::expected<QByteArray, QString> GitHubLink::get(const QUrl &url, const QByteArray &accept, const QString &missing) const {
    const QTemporaryDir scratch;
    const QString file = scratch.filePath(u"answer"_s);
    std::optional<PluginSources::Reply> answer;
    QEventLoop loop;
    // Into a file, so the plugins' limit on a download holds rather than the
    // few megabytes an answer kept in memory may have.
    m_sources->get(url, {{"Accept", accept}, {"X-GitHub-Api-Version", "2022-11-28"}}, file, 0, false,
                   [&answer, &loop](const PluginSources::Reply &reply) {
                       answer = reply;
                       loop.quit();
                   });
    if (!answer)
        loop.exec();
    if (!answer)
        return std::unexpected(u"GitHub gave no answer, so nothing was fetched."_s);
    const PluginSources::Reply &reply = *answer;
    if (!reply.error.isEmpty())
        return std::unexpected(reply.error);
    if (reply.status == 404)
        return std::unexpected(missing);
    if ((reply.status == 403 || reply.status == 429) && reply.headers.value("x-ratelimit-remaining") == "0")
        return std::unexpected(u"GitHub asked to be left alone for a while (it allows about 60 questions an hour without a "
                               "token), so nothing was fetched; try again later."_s);
    if (reply.status != 200)
        return std::unexpected(u"GitHub answered %1 to %2, so nothing was fetched."_s.arg(reply.status).arg(url.toDisplayString()));
    QFile read(file);
    if (!read.exists())
        return QByteArray();
    if (!read.open(QIODevice::ReadOnly))
        return std::unexpected(u"What GitHub sent couldn't be read back here."_s);
    return read.readAll();
}

std::expected<SpaceSnapshot, QString> GitHubLink::fetchGist(const QString &id) const {
    const auto answer = get(QUrl(m_service.api + "/gists/"_L1 + id), "application/vnd.github+json",
                            u"There is no gist %1: it may have been deleted, or the link was mistyped."_s.arg(id));
    if (!answer)
        return std::unexpected(answer.error());
    const QJsonObject gist = QJsonDocument::fromJson(*answer).object();
    const auto file = spaceFileOf(gist.value("files"_L1).toObject());
    if (!file)
        return std::unexpected(
            u"The gist %1 holds no shared collection (no %2 file in it)."_s.arg(id, QLatin1StringView(SpaceSnapshotFile::suffix)));
    // GitHub sends a large file's start only, and where the whole is.
    if (file->value("truncated"_L1).toBool()) {
        const auto whole = get(QUrl(file->value("raw_url"_L1).toString()), "application/octet-stream",
                               u"The gist %1's file is gone from GitHub."_s.arg(id));
        if (!whole)
            return std::unexpected(whole.error());
        return snapshotIn(*whole, u"The gist %1"_s.arg(id));
    }
    return snapshotIn(file->value("content"_L1).toString().toUtf8(), u"The gist %1"_s.arg(id));
}

std::expected<SpaceSnapshot, QString> GitHubLink::fetch(const QString &address) {
    const auto parsed = parse(address);
    if (!parsed)
        return std::unexpected(parsed.error());
    if (parsed->isGist())
        return fetchGist(parsed->gist);
    QUrl url(m_service.api + u"/repos/%1/%2/contents/%3"_s.arg(parsed->owner, parsed->repo, encodedPath(parsed->path)));
    url.setQuery(u"ref="_s + QString::fromLatin1(QUrl::toPercentEncoding(parsed->ref)));
    const auto answer = get(url, "application/vnd.github.raw",
                            u"There is no %1: it may have moved, or the repository is private or gone."_s.arg(where(*parsed)));
    if (!answer)
        return std::unexpected(answer.error());
    return snapshotIn(*answer, where(*parsed));
}

std::expected<GitHubLink::Answer, QString> GitHubLink::gh(const QStringList &arguments, const QByteArray &input) const {
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(u"GH_PROMPT_DISABLED"_s, u"1"_s); // it is not at a terminal to ask anything
    environment.insert(u"NO_COLOR"_s, u"1"_s);
    process.setProcessEnvironment(environment);
    process.start(m_gh, arguments);
    if (!process.waitForStarted())
        return std::unexpected(u"Sharing to a gist goes through GitHub's command line, gh, and there is none here: install it "
                               "(https://cli.github.com), sign in with gh auth login, and share again."_s);
    // The event loop goes on meanwhile, for whatever answers on this thread.
    QEventLoop loop;
    QObject::connect(&process, &QProcess::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(ghTakes, &loop, [&process] { process.kill(); });
    process.write(input);
    process.closeWriteChannel();
    if (process.state() != QProcess::NotRunning)
        loop.exec();
    if (process.exitStatus() == QProcess::CrashExit)
        return std::unexpected(u"gh stopped before it answered, so whether the gist was sent isn't known; share again to be sure."_s);
    return Answer{.exit = process.exitCode(),
                  .output = process.readAllStandardOutput(),
                  .errors = QString::fromUtf8(process.readAllStandardError()).trimmed()};
}

std::expected<SpaceTransport::Sent, QString> GitHubLink::send(const QString &gist, const SpaceSnapshot &snapshot,
                                                              const Sending &sending) const {
    const QJsonObject files{{QLatin1StringView(fileName), QJsonObject{{"content", QString::fromUtf8(SpaceSnapshotFile::write(snapshot))}}}};
    QJsonObject body{{"description", u"%1: a collection shared from Omabook"_s.arg(snapshot.name)}, {"files", files}};
    if (gist.isEmpty())
        body.insert("public"_L1, sending.open);
    const auto answer = gh({u"api"_s, u"--method"_s, gist.isEmpty() ? u"POST"_s : u"PATCH"_s,
                            gist.isEmpty() ? u"gists"_s : "gists/"_L1 + gist, u"--input"_s, u"-"_s},
                           QJsonDocument(body).toJson(QJsonDocument::Compact));
    if (!answer)
        return std::unexpected(answer.error());
    if (answer->exit == 4 || answer->errors.contains("gh auth login"_L1))
        return std::unexpected(u"gh isn't signed in to GitHub, so nothing was shared: run gh auth login, then share again."_s);
    if (answer->errors.contains("error connecting"_L1) || answer->errors.contains("could not resolve"_L1, Qt::CaseInsensitive))
        return std::unexpected(
            u"GitHub couldn't be reached, so nothing was shared (gh said: %1). Share again once it can be."_s.arg(answer->errors));
    if (answer->exit != 0 && !gist.isEmpty() && answer->errors.contains("HTTP 404"_L1))
        return Sent{}; // gone: the caller makes a new one
    if (answer->exit != 0)
        return std::unexpected(u"gh couldn't share the gist: %1"_s.arg(answer->errors.isEmpty() ? u"it gave no reason"_s : answer->errors));
    const QJsonObject made = QJsonDocument::fromJson(answer->output).object();
    const QString id = made.value("id"_L1).toString(gist);
    const QString link = made.value("html_url"_L1).toString();
    return Sent{.address = link.isEmpty() ? "https://gist.github.com/"_L1 + id : link,
                .created = gist.isEmpty(),
                .open = made.value("public"_L1).toBool(sending.open),
                .note = {}};
}

std::expected<SpaceTransport::Sent, QString> GitHubLink::publish(const SpaceSnapshot &snapshot, const Sending &sending) {
    if (const auto previous = parse(sending.previous); !sending.previous.isEmpty() && previous && previous->isGist()) {
        auto updated = send(previous->gist, snapshot, sending);
        if (!updated || !updated->address.isEmpty()) {
            if (updated && updated->open != sending.open)
                updated->note = u"A gist stays as it was made: this one is still %1."_s.arg(updated->open ? u"public"_s : u"secret"_s);
            return updated;
        }
        auto made = send({}, snapshot, sending);
        if (made)
            made->note =
                u"The gist it was shared to before, %1, is gone, so it went to a new one: give that link instead."_s.arg(sending.previous);
        return made;
    }
    return send({}, snapshot, sending);
}
