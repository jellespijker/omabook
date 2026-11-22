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
#include <ranges>

#include "log.h"

namespace {

using namespace Qt::StringLiterals;

struct Ran {
    int code = 0;
    QByteArray output;
};

// The last thing restic said on standard error: why it stopped.
[[nodiscard]] QString lastSaid(const QByteArray &errors) {
    const QStringList lines = QString::fromUtf8(errors).split(u'\n', Qt::SkipEmptyParts);
    return lines.isEmpty() ? QString() : lines.last().trimmed();
}

// Runs restic on the repository to its end; `allowed` is an exit code that
// still did what was asked (a backup that couldn't read a file, 3).
[[nodiscard]] std::expected<Ran, Failure> run(const ResticRepository &repository, const QStringList &arguments, int allowed = 0) {
    const QString program =
        QFileInfo(repository.program).isAbsolute() ? repository.program : QStandardPaths::findExecutable(repository.program);
    if (program.isEmpty() || !QFileInfo(program).isExecutable())
        return std::unexpected(Failure(Failure::Code::ResticMissing, {repository.program}));
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const QString &name : environment.keys())
        if (name.startsWith("RESTIC_"_L1))
            environment.remove(name);
    QProcess process;
    process.setProcessEnvironment(environment);
    process.start(program,
                  QStringList{u"--repo"_s, repository.location, u"--password-file"_s, repository.passwordFile, u"--retry-lock"_s, u"2m"_s}
                      + arguments);
    if (!process.waitForStarted())
        return std::unexpected(Failure(Failure::Code::ResticMissing, {repository.program}));
    process.closeWriteChannel();
    // As long as it takes: a first snapshot of a large library reads all of it.
    process.waitForFinished(-1);
    const QByteArray errors = process.readAllStandardError();
    const int code = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    if (code != 0 && code != allowed) {
        qCWarning(lcLibrary).noquote() << "restic" << arguments.value(0) << "failed:" << QString::fromUtf8(errors).trimmed();
        return std::unexpected(Failure(Failure::Code::ResticFailed, {lastSaid(errors)}));
    }
    return Ran{.code = code, .output = process.readAllStandardOutput()};
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

std::expected<QString, Failure> Restic::backup(const ResticRepository &repository, const Backup &backup) {
    QStringList arguments{u"backup"_s, u"--json"_s};
    for (const QString &tag : backup.tags)
        arguments << u"--tag"_s << tag;
    for (const QString &pattern : backup.excluding)
        arguments << u"--exclude"_s << pattern;
    const auto ran = run(repository, arguments + backup.paths, 3);
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

QString Restic::literal(const QString &path) {
    QString pattern;
    for (const QChar c : path) {
        if (c == u'*' || c == u'?' || c == u'[' || c == u'\\')
            pattern += u'\\';
        pattern += c;
    }
    return pattern;
}
