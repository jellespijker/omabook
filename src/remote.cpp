#include "remote.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>

Remote::Remote(const QString &target, const QStringList &ssh)
    : m_host(target.section(':', 0, 0).trimmed()), m_folder(target.contains(':') ? target.section(':', 1).trimmed() : QString()),
      // Never a password prompt nobody can see, and no wait on a computer that is off.
      m_ssh(ssh.isEmpty() ? QStringList{"ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10"} : ssh) {}

QString Remote::quote(const QString &argument) { return "'" + QString(argument).replace("'", "'\\''") + "'"; }

Remote::Result Remote::run(const QString &command, const QString &inputFile, const QString &outputFile) const {
    Result result;
    QProcess process;
    if (!inputFile.isEmpty())
        process.setStandardInputFile(inputFile);
    if (!outputFile.isEmpty())
        process.setStandardOutputFile(outputFile);
    // "--" ends ssh's own options, so a host can never be read as one.
    process.start(m_ssh.first(), m_ssh.mid(1) + QStringList{"--", m_host, command});
    if (!process.waitForStarted(5000)) {
        result.error = "ssh can't be started.";
        return result;
    }
    if (inputFile.isEmpty())
        process.closeWriteChannel();
    // A large book over a slow link takes a while; an hour is a hung link.
    if (!process.waitForFinished(3600 * 1000)) {
        process.kill();
        process.waitForFinished(2000);
        result.error = "The connection to " + m_host + " stopped answering.";
        return result;
    }
    result.code = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    result.output = process.readAllStandardOutput();
    result.error = QString::fromUtf8(process.readAllStandardError()).trimmed();
    return result;
}

Remote::Result Remote::omabook(const QStringList &arguments) const {
    // An ssh command doesn't get a login shell's PATH, and ~/.local/bin is
    // where a build of one's own lives.
    QString command = "PATH=\"$HOME/.local/bin:$PATH\" omabook";
    for (const QString &argument : arguments)
        command += " " + quote(argument);
    if (!m_folder.isEmpty())
        command += " --library " + quote(m_folder);
    return run(command);
}

bool Remote::fetch(const QString &remoteFile, const QString &localFile) const {
    return run("cat " + quote(remoteFile), {}, localFile).ok();
}

std::expected<void, QString> Remote::send(const QString &localFile, const QString &remoteFile) const {
    if (!QFileInfo(localFile).isReadable())
        return std::unexpected(QFileInfo(localFile).fileName() + " can't be read here");
    const Result sent = run("cat > " + quote(remoteFile), localFile);
    if (sent.ok())
        return {};
    return std::unexpected(sent.error.isEmpty() ? QString("copying %1 stopped").arg(QFileInfo(localFile).fileName())
                                                : sent.error.section('\n', -1));
}

std::expected<int, QString> Remote::handOver(const QJsonDocument &document, const QStringList &arguments) const {
    const QTemporaryDir here;
    QFile file(here.filePath("given.json"));
    if (!here.isValid() || !file.open(QIODevice::WriteOnly) || file.write(document.toJson(QJsonDocument::Compact)) <= 0)
        return std::unexpected(QString("it can't be written here"));
    file.close();
    const QString folder = QString::fromUtf8(run("mktemp -d").output).trimmed();
    if (folder.isEmpty())
        return std::unexpected("there is no room for it on " + m_host);
    const auto cleanup = qScopeGuard([&] { run("rm -rf " + quote(folder)); });
    if (const auto sent = send(file.fileName(), folder + "/given.json"); !sent)
        return std::unexpected(sent.error());
    const Result taken = omabook(arguments + QStringList{folder + "/given.json"});
    if (!taken.ok())
        return std::unexpected(taken.error.isEmpty() ? QString("it was not taken") : taken.error.section('\n', -1));
    return QJsonDocument::fromJson(taken.output).object().value("changed").toInt();
}
