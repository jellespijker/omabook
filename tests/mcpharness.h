#pragma once

// The real binary run as an agent meets it, for the tests: a machine of the
// test's own, and `omabook mcp` spoken to over standard input and output.
// Needs OMABOOK_BINARY.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>

#include "fixtures.h"

namespace McpHarness {

inline const QString modern = "2026-07-28";

// A machine of the test's own for the real binary to run on: a home with
// Omabook's AI off, a library, and a folder of stand-in programs that comes
// first on the PATH. Nothing of the user's is read or written.
struct Machine {
    QTemporaryDir dir;
    QString library;

    Machine() : library(dir.filePath("Library")) {
        QDir().mkpath(dir.filePath("home/.config/omabook"));
        QDir().mkpath(dir.filePath("bin"));
        Fixtures::write(dir.filePath("home/.config/omabook/config.json"), "{\"ai_provider\": \"off\"}");
    }

    QProcessEnvironment environment() const {
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert("HOME", dir.filePath("home"));
        environment.insert("XDG_CONFIG_HOME", dir.filePath("home/.config"));
        environment.insert("XDG_CACHE_HOME", dir.filePath("home/.cache"));
        environment.insert("PATH", dir.filePath("bin") + ":" + environment.value("PATH"));
        environment.insert("QT_QPA_PLATFORM", "offscreen");
        for (const char *unset : {"DISPLAY", "WAYLAND_DISPLAY", "ANTHROPIC_API_KEY"})
            environment.remove(unset);
        return environment;
    }

    // A stand-in program that writes down how it was run.
    void program(const QString &name, const QByteArray &body = {}) const {
        const QString path = dir.filePath("bin/" + name);
        Fixtures::write(path, "#!/bin/bash\necho \"$@\" >> '" + dir.filePath(name + ".ran").toUtf8() + "'\n" + body);
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }

    QString ran(const QString &name) const {
        QFile file(dir.filePath(name + ".ran"));
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
    }

    // Runs `omabook <arguments>` to the end.
    struct Ran {
        int code = -1;
        QString output, errors;
    };
    Ran omabook(const QStringList &arguments) const {
        QProcess process;
        process.setProcessEnvironment(environment());
        process.start(OMABOOK_BINARY, arguments);
        process.closeWriteChannel();
        Ran ran;
        if (!process.waitForFinished(30000))
            return ran;
        ran.code = process.exitCode();
        ran.output = QString::fromUtf8(process.readAllStandardOutput());
        ran.errors = QString::fromUtf8(process.readAllStandardError());
        return ran;
    }
};

// `omabook mcp`, spoken to a line at a time as a client would.
class Server {
public:
    explicit Server(const Machine &machine) {
        m_process.setProcessEnvironment(machine.environment());
        m_process.start(OMABOOK_BINARY, {"mcp", "--library", machine.library});
        m_process.waitForStarted();
    }
    ~Server() {
        m_process.closeWriteChannel();
        if (!m_process.waitForFinished(5000))
            m_process.kill();
    }
    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;

    void send(const QByteArray &text) {
        m_process.write(text + "\n");
        m_process.waitForBytesWritten();
    }

    // The next line the server writes; empty when it writes none in time.
    // Every line must be one JSON object: nothing but protocol on standard output.
    QJsonObject receive(int wait = 30000) {
        while (!m_process.canReadLine())
            if (!m_process.waitForReadyRead(wait))
                return {};
        const QByteArray text = m_process.readLine();
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(text, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
            m_noise << QString::fromUtf8(text);
        return document.object();
    }

    // One request the older way: after `initialize`, with nothing in `_meta`.
    QJsonObject request(const QString &method, const QJsonObject &params = {}) {
        send(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", ++m_id}, {"method", method}, {"params", params}})
                 .toJson(QJsonDocument::Compact));
        const QJsonObject answer = receive();
        if (answer.value("id").toInt() != m_id)
            m_noise << "an answer to another request: " + QString::fromUtf8(QJsonDocument(answer).toJson());
        return answer;
    }

    // One request the modern way: complete in itself.
    QJsonObject modernRequest(const QString &method, QJsonObject params = {}, const QString &version = modern) {
        params.insert("_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", version},
                                           {"io.modelcontextprotocol/clientInfo", QJsonObject{{"name", "tests"}, {"version", "1"}}},
                                           {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}});
        return request(method, params);
    }

    // The result of a tool call.
    QJsonObject call(const QString &tool, const QJsonObject &arguments = {}) {
        return request("tools/call", {{"name", tool}, {"arguments", arguments}}).value("result").toObject();
    }
    // What a tool that worked answered with; an empty object when it failed.
    QJsonObject data(const QString &tool, const QJsonObject &arguments = {}) {
        const QJsonObject result = call(tool, arguments);
        if (result.value("isError").toBool())
            m_noise << tool + " failed: " + result.value("content").toArray().first().toObject().value("text").toString();
        return result.value("structuredContent").toObject();
    }
    // Why a tool failed; empty when it worked.
    QString failure(const QString &tool, const QJsonObject &arguments = {}) {
        const QJsonObject result = call(tool, arguments);
        return result.value("isError").toBool() ? result.value("content").toArray().first().toObject().value("text").toString() : QString();
    }

    QString errors() { return QString::fromUtf8(m_process.readAllStandardError()); }
    QStringList noise() const { return m_noise; }

private:
    QProcess m_process;
    int m_id = 0;
    QStringList m_noise;
};

} // namespace McpHarness
