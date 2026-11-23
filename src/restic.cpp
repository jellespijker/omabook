#include "restic.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <algorithm>
#include <chrono>
#include <optional>
#include <ranges>

#include "log.h"

namespace {

using namespace Qt::StringLiterals;

struct Ran {
    int code = 0;
    QByteArray output;
    QByteArray errors;
};

// The last thing restic said on standard error: why it stopped.
[[nodiscard]] QString lastSaid(const QByteArray &errors) {
    const QStringList lines = QString::fromUtf8(errors).split(u'\n', Qt::SkipEmptyParts);
    return lines.isEmpty() ? QString() : lines.last().trimmed();
}

// The repository restic copies from (--from-repo), for a copy or an init
// like it: its options, after the command's name, and its password, handed
// over as the repository's own is.
struct From {
    QStringList options;
    QList<std::pair<QString, QByteArray>> environment;
};

[[nodiscard]] From fromOf(const ResticRepository &from) {
    From given{.options = {u"--from-repo"_s, from.location}, .environment = from.environment};
    if (from.password.isEmpty())
        given.options << u"--from-password-file"_s << from.passwordFile;
    else
        given.environment.append({u"RESTIC_FROM_PASSWORD"_s, from.password});
    return given;
}

// How a call runs: `allowed` is an exit code that still did what was asked
// (a backup that couldn't read a file, 3), `from` a repository it copies
// from, and `within` how long it may take, as long as it takes when none.
struct Call {
    int allowed = 0;
    From from;
    std::optional<std::chrono::seconds> within;
};

// Runs restic on the repository to its end.
[[nodiscard]] std::expected<Ran, Failure> run(const ResticRepository &repository, QStringList arguments, const Call &call = {}) {
    const QString program =
        QFileInfo(repository.program).isAbsolute() ? repository.program : QStandardPaths::findExecutable(repository.program);
    if (program.isEmpty() || !QFileInfo(program).isExecutable())
        return std::unexpected(Failure(Failure::Code::ResticMissing, {repository.program}));
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const QString &name : environment.keys())
        if (name.startsWith("RESTIC_"_L1))
            environment.remove(name);
    QStringList options{u"--repo"_s, repository.location, u"--retry-lock"_s, u"2m"_s};
    if (repository.password.isEmpty())
        options << u"--password-file"_s << repository.passwordFile;
    else
        environment.insert(u"RESTIC_PASSWORD"_s, QString::fromUtf8(repository.password));
    for (const auto &[name, value] : repository.environment + call.from.environment)
        environment.insert(name, QString::fromUtf8(value));
    for (qsizetype i = 0; i < call.from.options.size(); ++i)
        arguments.insert(1 + i, call.from.options.at(i));
    QProcess process;
    process.setProcessEnvironment(environment);
    process.start(program, options + arguments);
    if (!process.waitForStarted())
        return std::unexpected(Failure(Failure::Code::ResticMissing, {repository.program}));
    process.closeWriteChannel();
    // As long as it takes, unless told otherwise: a first snapshot of a large library reads all of it.
    if (!process.waitForFinished(call.within ? static_cast<int>(std::chrono::milliseconds(*call.within).count()) : -1)) {
        process.kill();
        process.waitForFinished();
        return std::unexpected(Failure(Failure::Code::ResticNoAnswer,
                                       {repository.location, QString::number(call.within.value_or(std::chrono::seconds(0)).count())}));
    }
    const QByteArray errors = process.readAllStandardError();
    const int code = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    if (code != 0 && code != call.allowed) {
        qCWarning(lcLibrary).noquote() << "restic" << arguments.value(0) << "failed:" << QString::fromUtf8(errors).trimmed();
        return std::unexpected(Failure(Failure::Code::ResticFailed, {lastSaid(errors)}));
    }
    return Ran{.code = code, .output = process.readAllStandardOutput(), .errors = errors};
}

// restic's times have nanoseconds, which Qt doesn't read.
[[nodiscard]] QDateTime timeOf(QString text) {
    static const QRegularExpression fraction(u"(\\.\\d{3})\\d+"_s);
    text.replace(fraction, u"\\1"_s);
    return QDateTime::fromString(text, Qt::ISODateWithMs).toLocalTime();
}

[[nodiscard]] QStringList strings(const QJsonArray &array) {
    QStringList list;
    for (const QJsonValue value : array)
        list << value.toString();
    return list;
}

} // namespace

QString Restic::Snapshot::tagged(const QString &prefix) const {
    const auto found = std::ranges::find_if(tags, [&prefix](const QString &tag) { return tag.startsWith(prefix); });
    return found == tags.end() ? QString() : found->mid(prefix.size());
}

std::expected<void, Failure> Restic::init(const ResticRepository &repository) {
    return run(repository, {u"init"_s}).transform([](const Ran &) {});
}

std::expected<void, Failure> Restic::init(const ResticRepository &repository, const ResticRepository &like) {
    return run(repository, {u"init"_s, u"--copy-chunker-params"_s}, {.allowed = 0, .from = fromOf(like), .within = {}})
        .transform([](const Ran &) {});
}

std::expected<void, Failure> Restic::copy(const ResticRepository &to, const ResticRepository &from, const QStringList &tags) {
    QStringList arguments{u"copy"_s};
    if (!tags.isEmpty())
        arguments << u"--tag"_s << tags.join(u',');
    return run(to, arguments, {.allowed = 0, .from = fromOf(from), .within = {}}).transform([](const Ran &) {});
}

std::expected<void, Failure> Restic::answers(const ResticRepository &repository, std::chrono::seconds within) {
    return run(repository, {u"cat"_s, u"config"_s}, {.allowed = 0, .from = {}, .within = within}).transform([](const Ran &) {});
}

std::expected<qint64, Failure> Restic::size(const ResticRepository &repository) {
    return run(repository, {u"stats"_s, u"--json"_s, u"--mode"_s, u"raw-data"_s}).transform([](const Ran &ran) {
        return QJsonDocument::fromJson(ran.output).object().value("total_size"_L1).toInteger(-1);
    });
}

std::expected<QString, Failure> Restic::backup(const ResticRepository &repository, const Backup &backup) {
    QStringList arguments{u"backup"_s, u"--json"_s};
    for (const QString &tag : backup.tags)
        arguments << u"--tag"_s << tag;
    for (const QString &pattern : backup.excluding)
        arguments << u"--exclude"_s << pattern;
    const auto ran = run(repository, arguments + backup.paths, {.allowed = 3, .from = {}, .within = {}});
    if (!ran)
        return std::unexpected(ran.error());
    // A line of JSON per message; the summary names the snapshot.
    for (const QByteArray &line : ran->output.split('\n')) {
        const QJsonObject message = QJsonDocument::fromJson(line).object();
        if (message.value("message_type"_L1).toString() == "summary"_L1 && message.contains("snapshot_id"_L1))
            return message.value("snapshot_id"_L1).toString();
    }
    // What it said last, which named no snapshot.
    return std::unexpected(Failure(Failure::Code::ResticFailed, {QString::fromUtf8(ran->output.trimmed().split('\n').last())}));
}

std::expected<QList<Restic::Snapshot>, Failure> Restic::snapshots(const ResticRepository &repository, const QStringList &tags) {
    QStringList arguments{u"snapshots"_s, u"--json"_s};
    if (!tags.isEmpty())
        arguments << u"--tag"_s << tags.join(u',');
    const auto ran = run(repository, arguments);
    if (!ran)
        return std::unexpected(ran.error());
    QList<Snapshot> found;
    for (const QJsonValue value : QJsonDocument::fromJson(ran->output).array()) {
        const QJsonObject snapshot = value.toObject();
        found << Snapshot{.id = snapshot.value("id"_L1).toString(),
                          .time = timeOf(snapshot.value("time"_L1).toString()),
                          .tags = strings(snapshot.value("tags"_L1).toArray()),
                          .paths = strings(snapshot.value("paths"_L1).toArray())};
    }
    std::ranges::stable_sort(found, {}, &Snapshot::time);
    return found;
}

std::expected<void, Failure> Restic::forget(const ResticRepository &repository, const QStringList &ids) {
    if (ids.isEmpty())
        return {};
    return run(repository, QStringList{u"forget"_s, u"--prune"_s} + ids).transform([](const Ran &) {});
}

std::expected<void, Failure> Restic::restore(const ResticRepository &repository, const QString &id, const QString &target,
                                             const QStringList &including) {
    QStringList arguments{u"restore"_s, id, u"--target"_s, target};
    for (const QString &path : including)
        arguments << u"--include"_s << literal(path);
    return run(repository, arguments).transform([](const Ran &) {});
}

std::expected<void, Failure> Restic::check(const ResticRepository &repository, const QString &readData) {
    QStringList arguments{u"check"_s};
    if (readData == "all"_L1)
        arguments << u"--read-data"_s;
    else if (!readData.isEmpty())
        arguments << u"--read-data-subset="_s + readData;
    // 1 is a repository with errors, said on standard output; the rest stopped it.
    const auto ran = run(repository, arguments, {.allowed = 1, .from = {}, .within = std::nullopt});
    if (!ran)
        return std::unexpected(ran.error());
    if (ran->code == 0)
        return {};
    const QStringList lines = QString::fromUtf8(ran->output).split(u'\n', Qt::SkipEmptyParts);
    const auto wrong = std::ranges::find_if(lines, [](const QString &line) { return line.contains("error"_L1, Qt::CaseInsensitive); });
    const QString said = wrong != lines.end() ? wrong->trimmed() : lastSaid(ran->errors);
    qCWarning(lcLibrary).noquote() << "restic check of" << repository.location
                                   << "found errors:" << QString::fromUtf8(ran->output).trimmed();
    return std::unexpected(Failure(Failure::Code::ResticFailed, {said}));
}

QString Restic::literal(const QString &path) {
    QString pattern;
    for (const QChar c : path) {
        if (c == u'*' || c == u'?' || c == u'[' || c == u'\\')
            pattern += u'\\';
        pattern += c;
    }
    return pattern;
}
