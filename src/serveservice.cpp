#include "serveservice.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>

using namespace Qt::StringLiterals;

ServeService::Where ServeService::usual() {
    return {.folder = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + u"/systemd/user"_s,
            .systemctl = u"systemctl"_s,
            .omabook = QCoreApplication::applicationFilePath()};
}

QString ServeService::unitPath() const { return QDir(m_where.folder).filePath(QString::fromLatin1(unit)); }

QString ServeService::unitText() const {
    // Quoted, as systemd takes a path with a space in it.
    return u"[Unit]\n"
           "Description=Omabook: the library for e-readers and phones\n"
           "After=network-online.target\n"
           "\n"
           "[Service]\n"
           "ExecStart=\"%1\" serve --service\n"
           "Restart=on-failure\n"
           "RestartSec=15\n"
           "\n"
           "[Install]\n"
           "WantedBy=default.target\n"_s.arg(m_where.omabook);
}

bool ServeService::installed() const { return QFile::exists(unitPath()); }

std::expected<void, Failure> ServeService::run(const QStringList &arguments) const {
    QProcess process;
    process.start(m_where.systemctl, QStringList{u"--user"_s} + arguments);
    if (!process.waitForStarted())
        return std::unexpected(Failure(Failure::Code::NoSystemd));
    if (!process.waitForFinished() || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString said = QString::fromUtf8(process.readAllStandardError()).trimmed();
        return std::unexpected(Failure(Failure::Code::ServiceRefused, {said.isEmpty() ? arguments.join(' ') : said}));
    }
    return {};
}

std::expected<void, Failure> ServeService::install() const {
    QFile file(unitPath());
    const QByteArray text = unitText().toUtf8();
    if (!QDir().mkpath(m_where.folder) || !file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(text) != text.size())
        return std::unexpected(Failure(Failure::Code::ServiceUnwritable, {unitPath(), file.errorString()}));
    file.close();
    if (auto loaded = run({u"daemon-reload"_s}); !loaded)
        return loaded;
    return run({u"enable"_s, u"--now"_s, QString::fromLatin1(unit)});
}

std::expected<void, Failure> ServeService::remove() const {
    if (!installed())
        return {};
    // Stopped first, so that the port is free when this returns.
    if (auto stopped = run({u"disable"_s, u"--now"_s, QString::fromLatin1(unit)}); !stopped)
        return stopped;
    if (!QFile::remove(unitPath()))
        return std::unexpected(Failure(Failure::Code::ServiceUnwritable, {unitPath(), QFile(unitPath()).errorString()}));
    return run({u"daemon-reload"_s});
}

std::expected<void, Failure> ServeService::restart() const { return run({u"restart"_s, QString::fromLatin1(unit)}); }
