#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>

#include "cli.h"
#include "fixtures.h"
#include "library.h"

namespace {

const QString modern = "2026-07-28";

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
        return result.value("isError").toBool() ? result.value("content").toArray().first().toObject().value("text").toString()
                                                : QString();
    }

    QString errors() { return QString::fromUtf8(m_process.readAllStandardError()); }
    QStringList noise() const { return m_noise; }

private:
    QProcess m_process;
    int m_id = 0;
    QStringList m_noise;
};

} // namespace

// Omabook as an MCP server, met the way an agent meets it: the real binary,
// with no display, over standard input and output.
class McpTests : public QObject {
    Q_OBJECT

private slots:
    void theOlderHandshakeIsAnswered() {
        const Machine machine;
        Server server(machine);
        const QJsonObject hello = server.request("initialize", {{"protocolVersion", "2025-06-18"}, {"capabilities", QJsonObject{}},
                                                                {"clientInfo", QJsonObject{{"name", "tests"}, {"version", "1"}}}})
                                      .value("result").toObject();
        QCOMPARE(hello.value("protocolVersion").toString(), QString("2025-06-18"));
        QCOMPARE(hello.value("serverInfo").toObject().value("name").toString(), QString("omabook"));
        QVERIFY(!hello.value("serverInfo").toObject().value("version").toString().isEmpty());
        for (const char *capability : {"tools", "resources", "prompts"})
            QVERIFY2(hello.value("capabilities").toObject().value(capability).isObject(), capability);
        QVERIFY(hello.value("instructions").toString().contains("books_search"));
        // A client of that era is not sent the fields of a later one.
        QVERIFY(!hello.contains("resultType"));

        // A version the server doesn't know is answered with one it does.
        QCOMPARE(server.request("initialize", {{"protocolVersion", "1999-01-01"}}).value("result").toObject().value("protocolVersion").toString(),
                 QString("2025-11-25"));

        // A notification is not answered: the next line is the ping's.
        server.send("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
        server.send("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":1}}");
        const QJsonObject pong = server.request("ping");
        QVERIFY(pong.contains("result"));
        QVERIFY(pong.value("result").toObject().isEmpty());
        QVERIFY2(server.noise().isEmpty(), qPrintable(server.noise().join('\n')));
    }

    void theModernWayNeedsNoHandshake() {
        const Machine machine;
        Server server(machine);
        // The first thing said can be anything; discovery says what is spoken.
        const QJsonObject found = server.modernRequest("server/discover").value("result").toObject();
        QCOMPARE(found.value("resultType").toString(), QString("complete"));
        QVERIFY(found.value("supportedVersions").toArray().contains(modern));
        QVERIFY(found.value("supportedVersions").toArray().contains("2025-11-25"));
        QVERIFY(found.value("capabilities").toObject().value("tools").isObject());
        QVERIFY(found.value("ttlMs").toInt(-1) >= 0);
        QCOMPARE(found.value("cacheScope").toString(), QString("public"));
        QCOMPARE(found.value("_meta").toObject().value("io.modelcontextprotocol/serverInfo").toObject().value("name").toString(),
                 QString("omabook"));

        const QJsonObject listed = server.modernRequest("tools/list").value("result").toObject();
        QCOMPARE(listed.value("resultType").toString(), QString("complete"));
        QVERIFY(listed.contains("ttlMs") && listed.contains("cacheScope"));
        QVERIFY(!listed.value("tools").toArray().isEmpty());

        const QJsonObject called = server.modernRequest("tools/call", {{"name", "library_list"}}).value("result").toObject();
        QCOMPARE(called.value("resultType").toString(), QString("complete"));
        QCOMPARE(called.value("isError").toBool(true), false);
        QVERIFY(!called.contains("ttlMs"));

        // A version the server doesn't speak: it says which it does.
        const QJsonObject refused = server.modernRequest("tools/list", {}, "2031-01-01").value("error").toObject();
        QCOMPARE(refused.value("code").toInt(), -32022);
        QCOMPARE(refused.value("data").toObject().value("requested").toString(), QString("2031-01-01"));
        QVERIFY(refused.value("data").toObject().value("supported").toArray().contains(modern));

        // A modern request must carry the client's capabilities.
        const QJsonObject bare = server.request("tools/list", {{"_meta", QJsonObject{{"io.modelcontextprotocol/protocolVersion", modern}}}});
        QCOMPARE(bare.value("error").toObject().value("code").toInt(), -32602);
        QVERIFY2(server.noise().isEmpty(), qPrintable(server.noise().join('\n')));
    }

    void everyToolIsAlsoACommand() {
        // Whatever an agent can do over MCP, a person or a script can do with
        // the command line. A new tool names its command here, or fails this.
        static const QHash<QString, QString> commands{
            {"omabook_info", "info"}, {"library_list", "libraries"}, {"library_switch", "libraries"},
            {"library_forget", "libraries"}, {"logs_read", "logs"}, {"books_search", "list"}, {"book_get", "show"}, {"book_add", "add"},
            {"book_add_format", "add"}, {"book_add_entry", "add"}, {"book_set_details", "set"},
            {"book_set_reading", "set"}, {"book_remove", "remove"}, {"book_export", "export"},
            {"book_convert", "convert"}, {"book_contents", "toc"}, {"book_text", "text"}, {"book_find", "find"},
            {"progress_get", "progress"}, {"progress_set", "progress"}, {"bookmarks_list", "bookmarks"},
            {"bookmark_add", "bookmarks"}, {"bookmark_remove", "bookmarks"}, {"notes_get", "notes"},
            {"lookup_candidates", "lookup"}, {"lookup_apply", "lookup"}, {"tidy_propose", "tidy"},
            {"tidy_apply", "tidy"}, {"ask_book", "ask"}, {"ask_library", "ask"}, {"suggest_reading", "suggest"},
            {"catalog_export", "catalog"}, {"sync_status", "sync"}, {"sync_run", "sync"}, {"plugins_list", "plugins"},
            {"plugin_set_enabled", "plugins"}, {"customize_get", "customize"}, {"customize_set", "change"},
            {"customize_undo", "change"}, {"plugin_api", "plugin"}, {"plugin_list", "plugin"},
            {"plugin_write", "plugin"}, {"plugin_check", "plugin"}, {"plugin_run", "plugin"},
            {"plugin_enable", "plugin"}, {"plugin_install", "plugin"}, {"plugin_remove", "plugin"},
            {"plugin_sources", "plugin"}, {"plugin_add", "plugin"}, {"plugin_update", "plugin"},
            {"plugin_forget_source", "plugin"}, {"plugin_search", "plugin"},
            {"calibre_libraries", "calibre"}, {"calibre_plugins", "calibre"}, {"calibre_import", "calibre"},
            {"reading_site_shelves", "goodreads"}, {"reading_site_sync", "goodreads"},
            {"goodreads_import", "goodreads"}, {"goodreads_export", "goodreads"},
            {"reading_sites_list", "sites"}, {"reading_sites_sync", "sites"}, {"reading_site_disconnect", "sites"},
            {"graph_find", "graph"}, {"graph_node", "graph"}, {"graph_book", "graph"}, {"graph_scope", "graph"},
            {"graph_scopes", "graph"}, {"graph_add", "graph"}, {"graph_link", "graph"}, {"graph_unlink", "graph"},
            {"graph_merge", "graph"}, {"graph_remove", "graph"}, {"graph_build", "graph"}, {"graph_export", "graph"},
            {"graph_copy_scope", "graph"}, {"graph_remove_scope", "graph"}, {"graph_reading_log", "graph"}, {"graph_connect", "graph"},
            {"themes_list", "theme"}, {"theme_use", "theme"},
            {"reading_format_get", "format"}, {"reading_format_set", "format"},
            {"collections_list", "collections"}, {"collection_get", "collections"}, {"collection_add", "collections"},
            {"collection_set_rule", "collections"}, {"collection_rename", "collections"}, {"collection_move", "collections"},
            {"collection_put", "collections"}, {"collection_remove", "collections"}, {"collection_format", "collections"},
            {"collections_sort", "collections"}, {"collections_sort_apply", "collections"},
        };
        const Machine machine;
        Server server(machine);
        for (const QJsonValue &value : server.request("tools/list").value("result").toObject().value("tools").toArray()) {
            const QString tool = value.toObject().value("name").toString();
            QVERIFY2(commands.contains(tool), qPrintable(tool + " has no command named for it"));
            QVERIFY2(isCliCommand(commands.value(tool)), qPrintable(commands.value(tool)));
        }
    }

    void everyToolDescribesItself() {
        const Machine machine;
        Server server(machine);
        const QJsonArray tools = server.request("tools/list").value("result").toObject().value("tools").toArray();
        QVERIFY(tools.size() >= 30);
        static const QRegularExpression name("^[A-Za-z0-9_.-]{1,128}$");
        const QSet<QString> types{"string", "integer", "number", "boolean", "array", "object"};
        QStringList names;
        for (const QJsonValue &value : tools) {
            const QJsonObject tool = value.toObject();
            const QByteArray called = tool.value("name").toString().toUtf8();
            auto of = [&](const QString &member) { return called + " " + member.toUtf8(); };
            names << tool.value("name").toString();
            QVERIFY2(name.match(tool.value("name").toString()).hasMatch(), called.constData());
            QVERIFY2(!tool.value("title").toString().isEmpty(), called.constData());
            QVERIFY2(tool.value("description").toString().size() > 20, called.constData());

            // The input schema is one an agent can fill in from its words alone.
            const QJsonObject schema = tool.value("inputSchema").toObject();
            QVERIFY2(schema.value("type").toString() == "object", called.constData());
            const QJsonObject properties = schema.value("properties").toObject();
            for (auto property = properties.begin(); property != properties.end(); ++property) {
                const QJsonObject described = property.value().toObject();
                // A property with no type takes any JSON value; one with a type names a real one.
                QVERIFY2(!described.contains("type") || types.contains(described.value("type").toString()),
                         of(property.key()).constData());
                QVERIFY2(!described.value("description").toString().isEmpty(), of(property.key()).constData());
                if (described.value("type").toString() == "array")
                    QVERIFY2(described.value("items").isObject(), of(property.key()).constData());
            }
            for (const QJsonValue &required : schema.value("required").toArray())
                QVERIFY2(properties.contains(required.toString()), of("requires " + required.toString()).constData());

            // What it does to the world is said, in the protocol's own hints.
            const QJsonObject annotations = tool.value("annotations").toObject();
            QVERIFY2(annotations.value("readOnlyHint").isBool(), called.constData());
            QVERIFY2(annotations.value("openWorldHint").isBool(), called.constData());
            QVERIFY2(annotations.value("readOnlyHint").toBool() != annotations.value("destructiveHint").isBool(), called.constData());
        }
        // In one order, each name once, so the list can be cached.
        QStringList sorted = names;
        sorted.sort();
        QCOMPARE(names, sorted);
        QCOMPARE(QSet<QString>(names.begin(), names.end()).size(), names.size());

        auto hint = [&](const QString &tool, const QString &which) {
            for (const QJsonValue &value : tools)
                if (value.toObject().value("name").toString() == tool)
                    return value.toObject().value("annotations").toObject().value(which);
            return QJsonValue();
        };
        // What takes something away says so; what only reads says that.
        for (const char *tool : {"book_remove", "bookmark_remove", "tidy_apply", "lookup_apply", "book_add_format"}) {
            QVERIFY2(hint(tool, "destructiveHint").toBool(false), tool);
            QVERIFY2(!hint(tool, "readOnlyHint").toBool(true), tool);
        }
        for (const char *tool : {"books_search", "book_get", "book_text", "book_find", "book_contents", "progress_get",
                                 "bookmarks_list", "notes_get", "tidy_propose", "plugins_list", "customize_get", "library_list"})
            QVERIFY2(hint(tool, "readOnlyHint").toBool(false), tool);
        for (const char *tool : {"book_add", "book_add_entry", "book_set_details", "book_set_reading", "progress_set",
                                 "customize_set", "library_switch"}) {
            QVERIFY2(!hint(tool, "readOnlyHint").toBool(true), tool);
            QVERIFY2(hint(tool, "destructiveHint").isBool() && !hint(tool, "destructiveHint").toBool(), tool);
        }
        // What leaves this computer says that too.
        QVERIFY(hint("lookup_candidates", "openWorldHint").toBool());
        QVERIFY(!hint("books_search", "openWorldHint").toBool(true));
    }

    void anAgentNamesBooksElsewhereAndTakesCandidatesById() {
        const Machine machine;
        // Two catalogs of the test's own and nothing else, so no network is asked.
        for (const char *plugin : {"builtin:Open Library", "builtin:Google Books", "builtin:Apple Books", "builtin:Crossref",
                                   "builtin:Goodreads", "builtin:Hardcover", "calibre"})
            QCOMPARE(machine.omabook({"plugins", "disable", plugin}).code, 0);
        for (const QString name : {"alpha", "beta"}) {
            const QString folder = machine.dir.filePath("home/.config/omabook/plugins/" + name);
            QVERIFY(QDir().mkpath(folder));
            QVERIFY(Fixtures::write(folder + "/lookup", "#!/bin/sh\ncat > /dev/null\necho '{\"candidates\": [{\"title\": \"From "
                                                            + name.toUtf8() + "\", \"authors\": [\"Ann Author\"]}]}'\n"));
            QVERIFY(QFile::setPermissions(folder + "/lookup", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
            QVERIFY(Fixtures::write(folder + "/plugin.json", "{\"name\": \"" + name.toUtf8() + "\", \"fields\": [\"title\"], "
                                                                 "\"command\": [\"" + folder.toUtf8() + "/lookup\"]}"));
        }
        QVERIFY(Fixtures::epub(machine.dir.filePath("first.epub"), "The Test Book"));
        Server server(machine);
        const qint64 id = server.data("book_add", {{"paths", QJsonArray{machine.dir.filePath("first.epub")}}})
                              .value("added").toArray().first().toObject().value("id").toInteger();
        QVERIFY(id > 0);

        // Identifiers: added and changed, forgotten when empty, never "generated".
        QJsonObject book = server.data("book_set_details", {{"id", id}, {"identifiers", QJsonObject{{"goodreads", "4671"}, {"isbn", "978"}}}});
        QCOMPARE(book.value("identifiers").toObject().value("goodreads").toString(), QString("4671"));
        book = server.data("book_set_details", {{"id", id}, {"identifiers", QJsonObject{{"goodreads", "5000"}, {"isbn", ""}}}});
        QCOMPARE(book.value("identifiers").toObject(), (QJsonObject{{"goodreads", "5000"}}));
        QVERIFY(server.failure("book_set_details", {{"id", id}, {"title", "Nope"}, {"identifiers", QJsonObject{{"generated", "mine"}}}})
                    .contains("generated"));
        QCOMPARE(server.data("book_get", {{"id", id}}).value("title").toString(), QString("The Test Book"));
        QVERIFY(server.failure("book_set_details", {{"id", id}, {"identifiers", "goodreads=1"}}).contains("object"));

        // Candidates carry their number and an id; the id is what is applied.
        const QJsonArray candidates = server.data("lookup_candidates", {{"id", id}}).value("candidates").toArray();
        QCOMPARE(candidates.size(), 2);
        QCOMPARE(candidates.at(0).toObject().value("number").toInt(), 1);
        QCOMPARE(candidates.at(1).toObject().value("source").toString(), QString("beta"));
        const QString beta = candidates.at(1).toObject().value("candidate_id").toString();
        QVERIFY2(beta.startsWith("beta-"), qPrintable(beta));
        QVERIFY(server.failure("lookup_apply", {{"id", id}, {"candidate_id", "beta-00000000"}}).contains("lookup_candidates again"));
        QVERIFY(server.failure("lookup_apply", {{"id", id}}).contains("candidate_id"));
        QCOMPARE(server.data("lookup_apply", {{"id", id}, {"candidate_id", beta}}).value("title").toString(), QString("From beta"));
        // Taken, the listing is done with.
        QVERIFY(!server.failure("lookup_apply", {{"id", id}, {"candidate_id", beta}}).isEmpty());
        QVERIFY2(server.noise().isEmpty(), qPrintable(server.noise().join('\n')));
    }

    void anAgentWorksTheLibrary() {
        const Machine machine;
        QVERIFY(Fixtures::epub(machine.dir.filePath("first.epub"), "The Test Book"));
        QVERIFY(Fixtures::write(machine.dir.filePath("Sea Story - Ann Sailor.txt"),
                                "CHAPTER 1\n\nIt was a dark night.\n\nCHAPTER 2\n\nThe whale appeared at last.\n"));
        Server server(machine);
        QVERIFY(server.request("initialize", {{"protocolVersion", "2025-11-25"}}).contains("result"));

        // Adding copies the files in and says which book each became.
        const QJsonArray added = server.data("book_add", {{"paths", QJsonArray{machine.dir.filePath("first.epub"),
                                                                              machine.dir.filePath("Sea Story - Ann Sailor.txt")}}})
                                     .value("added").toArray();
        QCOMPARE(added.size(), 2);
        const qint64 epub = added.at(0).toObject().value("id").toInteger(), sea = added.at(1).toObject().value("id").toInteger();
        QVERIFY(epub > 0 && sea > 0 && epub != sea);
        QVERIFY(QFile::exists(machine.dir.filePath("first.epub")));
        QVERIFY(server.data("book_add", {{"paths", QJsonArray{machine.dir.filePath("first.epub")}}})
                    .value("added").toArray().first().toObject().value("duplicate").toBool());

        // A paper book and one to read some day have no file.
        const qint64 paper = server.data("book_add_entry", {{"title", "Moby-Dick"}, {"authors", QJsonArray{"Herman Melville"}},
                                                           {"paper", true}, {"status", "read"}, {"rating", 5}})
                                 .value("id").toInteger();
        const qint64 wish = server.data("book_add_entry", {{"title", "Ulysses"}, {"paper", false}, {"status", "to-read"}})
                                .value("id").toInteger();
        QVERIFY(paper > 0 && wish > 0);

        // The list comes in pages, small, with ids.
        QJsonObject page = server.data("books_search", {{"limit", 3}});
        QCOMPARE(page.value("total").toInt(), 4);
        QCOMPARE(page.value("books").toArray().size(), 3);
        QCOMPARE(page.value("next_offset").toInt(), 3);
        QVERIFY(!page.value("books").toArray().first().toObject().contains("description"));
        page = server.data("books_search", {{"limit", 3}, {"offset", 3}});
        QCOMPARE(page.value("books").toArray().size(), 1);
        QVERIFY(!page.contains("next_offset"));
        // The search language, with what says where a book stands.
        auto found = [&](const QString &query) {
            QList<qint64> ids;
            for (const QJsonValue &book : server.data("books_search", {{"query", query}}).value("books").toArray())
                ids << book.toObject().value("id").toInteger();
            return ids;
        };
        QCOMPARE(found("author:sailor"), QList<qint64>{sea});
        QCOMPARE(found("is:paper"), QList<qint64>{paper});
        QCOMPARE(found("is:wishlist"), QList<qint64>{wish});
        QCOMPARE(found("status:read"), QList<qint64>{paper});
        QCOMPARE(found("rating:5"), QList<qint64>{paper});
        QVERIFY(found("is:digital").contains(epub) && found("is:digital").contains(sea));

        // One book, whole.
        QJsonObject book = server.data("book_get", {{"id", epub}});
        QCOMPARE(book.value("title").toString(), QString("The Test Book"));
        QVERIFY(QFile::exists(book.value("files").toObject().value("epub").toString()));

        // Details: only what is given changes.
        book = server.data("book_set_details", {{"id", sea}, {"series", "The Deep"}, {"series_index", 2}, {"tags", QJsonArray{"sea", "whales"}}});
        QCOMPARE(book.value("title").toString(), QString("Sea Story"));
        QCOMPARE(book.value("series").toString(), QString("The Deep"));
        QCOMPARE(book.value("tags").toArray().size(), 2);
        QVERIFY(server.failure("book_set_details", {{"id", sea}, {"title", "  "}}).contains("needs a title"));

        // Reading status and stars.
        book = server.data("book_set_reading", {{"id", sea}, {"status", "reading"}, {"rating", 4}});
        QCOMPARE(book.value("status").toString(), QString("reading"));
        QCOMPARE(book.value("rating").toInt(), 4);
        book = server.data("book_set_reading", {{"id", sea}, {"finished", "2026-03-01"}, {"status", "read"}});
        QCOMPARE(book.value("rating").toInt(), 4);
        QCOMPARE(server.data("book_get", {{"id", sea}}).value("finished").toString(), QString("2026-03-01"));
        QVERIFY(server.failure("book_set_reading", {{"id", sea}, {"finished", "last week"}}).contains("ISO date"));

        // Reading: contents, a section in pieces, and finding words.
        const QJsonObject contents = server.data("book_contents", {{"id", epub}});
        QVERIFY(contents.value("sections").toInt() >= 2);
        QVERIFY(!contents.value("contents").toArray().isEmpty());
        QJsonObject text = server.data("book_text", {{"id", sea}, {"section", 0}, {"max_chars", 12}});
        QCOMPARE(text.value("text").toString().size(), 12);
        QCOMPARE(text.value("next_offset").toInt(), 12);
        const QString rest = server.data("book_text", {{"id", sea}, {"section", 0}, {"offset", 12}}).value("text").toString();
        QVERIFY(rest.contains("The whale appeared at last."));
        // A file that isn't in the library reads the same way.
        QVERIFY(server.data("book_text", {{"path", machine.dir.filePath("Sea Story - Ann Sailor.txt")}, {"section", 0}})
                    .value("text").toString().contains("dark night"));
        const QJsonObject matches = server.data("book_find", {{"id", sea}, {"words", "the WHALE"}});
        QCOMPARE(matches.value("matches").toArray().size(), 1);
        QVERIFY(matches.value("matches").toArray().first().toObject().value("snippet").toString().contains("whale appeared"));
        QVERIFY(server.data("book_find", {{"id", sea}, {"words", "kraken"}}).value("matches").toArray().isEmpty());
        QVERIFY(server.failure("book_text", {{"id", paper}, {"section", 0}}).contains("no file"));

        // The reading place, bookmarks and the companion's notes.
        QJsonObject place = server.data("progress_set", {{"id", epub}, {"section", 1}, {"fraction", 0.5}, {"percent", 75}});
        QCOMPARE(place.value("section").toInt(), 1);
        QVERIFY(place.value("updated").toInteger() > 0);
        QCOMPARE(server.data("progress_get", {{"id", epub}}).value("percent").toDouble(), 75.0);
        const qint64 mark = server.data("bookmark_add", {{"id", epub}, {"section", 1}, {"fraction", 0.25}, {"text", "Here"}, {"note", "Why"}})
                                .value("bookmark_id").toInteger();
        QVERIFY(mark > 0);
        QJsonArray marks = server.data("bookmarks_list", {{"id", epub}}).value("bookmarks").toArray();
        QCOMPARE(marks.size(), 1);
        QCOMPARE(marks.first().toObject().value("note").toString(), QString("Why"));
        QVERIFY(server.failure("bookmark_remove", {{"id", sea}, {"bookmark_id", mark}}).contains("no bookmark"));
        QCOMPARE(server.data("bookmark_remove", {{"id", epub}, {"bookmark_id", mark}}).value("removed").toInteger(), mark);
        QVERIFY(server.data("bookmarks_list", {{"id", epub}}).value("bookmarks").toArray().isEmpty());
        {
            // Notes are the companion's to write; an agent reads them.
            Library beside(machine.library);
            QVERIFY(beside.open());
            Recap recap;
            recap.section = 1;
            recap.text = "A test happens.";
            beside.setNotes(epub, "recap", recap);
        }
        const QJsonObject notes = server.data("notes_get", {{"id", epub}});
        QCOMPARE(notes.value("recap").toObject().value("text").toString(), QString("A test happens."));
        QCOMPARE(notes.value("cast").toObject().value("text").toString(), QString());
        QVERIFY(!server.data("notes_get", {{"id", epub}, {"kind", "cast"}}).contains("recap"));

        // A catalog, as text or as a file that is never written over.
        QVERIFY(server.data("catalog_export", {{"format", "bibtex"}, {"query", "sea"}}).value("catalog").toString().contains("@book{"));
        const QString csv = machine.dir.filePath("books.csv");
        QCOMPARE(server.data("catalog_export", {{"output_path", csv}}).value("books").toInt(), 4);
        QVERIFY(QFile::exists(csv));
        QVERIFY(server.failure("catalog_export", {{"output_path", csv}}).contains("already a file"));

        // A copy out of the library, likewise.
        const QString copy = server.data("book_export", {{"id", epub}, {"folder", machine.dir.filePath("out")}}).value("file").toString();
        QVERIFY(QFile::exists(copy));
        QVERIFY(server.failure("book_export", {{"id", epub}, {"folder", machine.dir.filePath("out")}}).contains("already a file"));

        // Removing takes the book away, and says which.
        QCOMPARE(server.data("book_remove", {{"id", wish}}).value("title").toString(), QString("Ulysses"));
        QVERIFY(server.failure("book_get", {{"id", wish}}).contains("no book"));
        QVERIFY2(server.noise().isEmpty(), qPrintable(server.noise().join('\n')));
    }

    void tidyingIsProposedThenChosen() {
        const Machine machine;
        QVERIFY(Fixtures::write(machine.dir.filePath("Twice - Reader, Rita.txt"), "One copy.\n"));
        QVERIFY(Fixtures::write(machine.dir.filePath("twice - Rita Reader.md"), "Another copy.\n"));
        Server server(machine);
        QCOMPARE(server.data("book_add", {{"paths", QJsonArray{machine.dir.filePath("Twice - Reader, Rita.txt"),
                                                                machine.dir.filePath("twice - Rita Reader.md")}}})
                     .value("added").toArray().size(), 2);

        const QJsonArray proposals = server.data("tidy_propose").value("proposals").toArray();
        QStringList kinds;
        for (const QJsonValue &proposal : proposals)
            kinds << proposal.toObject().value("kind").toString();
        QVERIFY2(kinds.contains("duplicate") && kinds.contains("author"), qPrintable(kinds.join(',')));
        // Proposing changed nothing.
        QCOMPARE(server.data("books_search").value("total").toInt(), 2);

        // Only what is handed back is done, in whatever order it comes.
        QJsonArray reversed;
        for (const QJsonValue &proposal : proposals)
            reversed.prepend(proposal);
        const QJsonObject done = server.data("tidy_apply", {{"proposals", reversed}});
        QCOMPARE(done.value("applied").toInt(), int(proposals.size()));
        QVERIFY(done.value("problems").toArray().isEmpty());
        const QJsonArray left = server.data("books_search").value("books").toArray();
        QCOMPARE(left.size(), 1);
        QCOMPARE(left.first().toObject().value("authors").toArray().first().toString(), QString("Rita Reader"));
        QCOMPARE(left.first().toObject().value("formats").toArray().size(), 2);
        QVERIFY(server.data("tidy_propose").value("proposals").toArray().isEmpty());
        QVERIFY(server.failure("tidy_apply", {{"proposals", QJsonArray{QJsonObject{{"kind", "burn"}, {"id", 1}}}}}).contains("kind"));
        QVERIFY2(server.noise().isEmpty(), qPrintable(server.noise().join('\n')));
    }

    void theWindowAndTheServerShareALibrary() {
        // The window holds the library open while an agent works on it: here
        // the test is the window, with a connection of its own.
        const Machine machine;
        QVERIFY(Fixtures::epub(machine.dir.filePath("first.epub"), "From The Window"));
        QVERIFY(Fixtures::write(machine.dir.filePath("Agent Book - Ann Agent.txt"), "Words.\n"));
        Library window(machine.library);
        QVERIFY(window.open());
        Server server(machine);
        QCOMPARE(server.data("books_search").value("total").toInt(), 0);

        const qint64 fromWindow = window.add(machine.dir.filePath("first.epub")).id;
        QVERIFY(fromWindow > 0);
        QCOMPARE(server.data("book_get", {{"id", fromWindow}}).value("title").toString(), QString("From The Window"));

        const qint64 fromAgent = server.data("book_add", {{"paths", QJsonArray{machine.dir.filePath("Agent Book - Ann Agent.txt")}}})
                                     .value("added").toArray().first().toObject().value("id").toInteger();
        QVERIFY(window.book(fromAgent).has_value());
        QCOMPARE(window.book(fromAgent)->meta.title, QString("Agent Book"));

        // Each sees the other's changes at once.
        QVERIFY(server.data("progress_set", {{"id", fromWindow}, {"percent", 40}}).contains("percent"));
        QCOMPARE(window.progress(fromWindow).percent, 40.0);
        Progress place = window.progress(fromWindow);
        place.percent = 60;
        place.updated = 0;
        window.setProgress(fromWindow, place);
        QCOMPARE(server.data("progress_get", {{"id", fromWindow}}).value("percent").toDouble(), 60.0);
        QVERIFY2(server.noise().isEmpty(), qPrintable(server.noise().join('\n')));
    }

    void whatGoesWrongIsSaidInTheResult() {
        const Machine machine;
        Server server(machine);
        // Something an agent can put right comes back as the tool's result.
        QJsonObject result = server.call("book_get", {{"id", 99}});
        QCOMPARE(result.value("isError").toBool(), true);
        QVERIFY(!result.contains("structuredContent"));
        QVERIFY(result.value("content").toArray().first().toObject().value("text").toString().contains("no book 99"));
        QCOMPARE(result.value("content").toArray().first().toObject().value("type").toString(), QString("text"));

        // Arguments are checked against the schema, in words.
        QVERIFY(server.failure("book_get").contains("\"id\" is required"));
        QVERIFY(server.failure("book_get", {{"id", "seven"}}).contains("whole number"));
        QVERIFY(server.failure("book_get", {{"id", 1.5}}).contains("whole number"));
        QVERIFY(server.failure("book_get", {{"id", 0}}).contains("at least 1"));
        QVERIFY(server.failure("book_get", {{"id", 1}, {"colour", "red"}}).contains("no argument \"colour\""));
        QVERIFY(server.failure("book_set_reading", {{"id", 1}, {"status", "abandoned"}}).contains("must be one of"));
        QVERIFY(server.failure("progress_set", {{"id", 1}, {"fraction", 2}}).contains("at most 1"));
        QVERIFY(server.failure("book_add", {{"paths", QJsonArray{7}}}).contains("paths[0]"));
        QVERIFY(server.failure("book_text", {{"section", 0}}).contains("one of the two"));
        QVERIFY(server.failure("book_text", {{"path", "/no/such/book.epub"}, {"section", 0}}).contains("no book or file"));
        QVERIFY(server.failure("library_switch", {{"folder", machine.dir.filePath("Nowhere")}}).contains("--library"));

        // With Omabook's AI off, what needs it says so.
        QVERIFY(Fixtures::write(machine.dir.filePath("Story - Ann.txt"), "Words.\n"));
        const qint64 id = server.data("book_add", {{"paths", QJsonArray{machine.dir.filePath("Story - Ann.txt")}}})
                              .value("added").toArray().first().toObject().value("id").toInteger();
        QVERIFY(server.failure("ask_book", {{"id", id}, {"question", "Who narrates?"}}).contains("AI is turned off"));
        QVERIFY(server.failure("ask_library", {{"question", "What do I have?"}}).contains("AI is turned off"));
        QVERIFY(server.failure("suggest_reading").contains("AI is turned off"));
        QVERIFY(server.failure("tidy_propose", {{"ai", true}}).contains("AI is turned off"));
        QCOMPARE(server.data("omabook_info").value("ai").toObject().value("source").toString(), QString("none"));

        // What only a programmer can put right is a protocol error.
        QJsonObject error = server.request("tools/call", {{"name", "book_burn"}}).value("error").toObject();
        QCOMPARE(error.value("code").toInt(), -32602);
        QVERIFY(error.value("message").toString().contains("book_burn"));
        QCOMPARE(server.request("tools/call").value("error").toObject().value("code").toInt(), -32602);
        QCOMPARE(server.request("tools/call", {{"name", "book_get"}, {"arguments", QJsonArray{1}}}).value("error").toObject().value("code").toInt(),
                 -32602);
        QCOMPARE(server.request("tools/list", {{"cursor", "page-2"}}).value("error").toObject().value("code").toInt(), -32602);
        QVERIFY2(server.noise().isEmpty(), qPrintable(server.noise().join('\n')));
    }

    void whatIsNotTheProtocolIsRefusedAndTheServerCarriesOn() {
        const Machine machine;
        Server server(machine);
        server.send("this is not JSON");
        QJsonObject answer = server.receive();
        QCOMPARE(answer.value("error").toObject().value("code").toInt(), -32700);
        QVERIFY(answer.value("id").isNull());
        QCOMPARE(answer.value("jsonrpc").toString(), QString("2.0"));

        server.send("[1, 2]");
        QCOMPARE(server.receive().value("error").toObject().value("code").toInt(), -32600);
        server.send("{\"id\": 5, \"method\": \"ping\"}");
        answer = server.receive();
        QCOMPARE(answer.value("error").toObject().value("code").toInt(), -32600);
        QCOMPARE(answer.value("id").toInt(), 5);
        server.send("{\"jsonrpc\": \"2.0\", \"id\": null, \"method\": \"ping\"}");
        QCOMPARE(server.receive().value("error").toObject().value("code").toInt(), -32600);
        server.send("{\"jsonrpc\": \"2.0\", \"id\": 6, \"method\": \"tools/list\", \"params\": [1]}");
        QCOMPARE(server.receive().value("error").toObject().value("code").toInt(), -32602);

        // A method there isn't, by number or by name.
        QJsonObject unknown = server.request("books/steal");
        QCOMPARE(unknown.value("error").toObject().value("code").toInt(), -32601);
        QVERIFY(unknown.value("error").toObject().value("message").toString().contains("books/steal"));
        server.send("{\"jsonrpc\": \"2.0\", \"id\": \"by-name\", \"method\": \"ping\"}");
        QCOMPARE(server.receive().value("id").toString(), QString("by-name"));
        // An empty line and a stray response are passed over in silence.
        server.send("");
        server.send("{\"jsonrpc\": \"2.0\", \"id\": 9, \"result\": {}}");
        QVERIFY(server.request("ping").contains("result"));
        QVERIFY(server.request("tools/list").value("result").toObject().value("tools").isArray());
    }

    void resourcesAndPromptsAreThere() {
        const Machine machine;
        QVERIFY(Fixtures::epub(machine.dir.filePath("first.epub"), "The Test Book"));
        Server server(machine);
        const qint64 id = server.data("book_add", {{"paths", QJsonArray{machine.dir.filePath("first.epub")}}})
                              .value("added").toArray().first().toObject().value("id").toInteger();

        const QJsonArray resources = server.request("resources/list").value("result").toObject().value("resources").toArray();
        QCOMPARE(resources.first().toObject().value("uri").toString(), QString("omabook://library/books"));
        QCOMPARE(server.request("resources/templates/list").value("result").toObject().value("resourceTemplates").toArray().size(), 3);
        auto read = [&](const QString &uri) {
            return server.request("resources/read", {{"uri", uri}}).value("result").toObject().value("contents").toArray().first().toObject();
        };
        QVERIFY(read("omabook://library/books").value("text").toString().contains("The Test Book"));
        QCOMPARE(read("omabook://library/books").value("mimeType").toString(), QString("application/json"));
        QVERIFY(read(QString("omabook://book/%1").arg(id)).value("text").toString().contains("\"files\""));
        QCOMPARE(read(QString("omabook://book/%1/section/0").arg(id)).value("mimeType").toString(), QString("text/plain"));
        QVERIFY(!read(QString("omabook://book/%1/section/0").arg(id)).value("text").toString().isEmpty());
        for (const QString &missing : {QString("omabook://book/77"), QString("omabook://book/%1/section/99").arg(id), QString("file:///etc/passwd")}) {
            const QJsonObject error = server.request("resources/read", {{"uri", missing}}).value("error").toObject();
            QCOMPARE(error.value("code").toInt(), -32602);
            QCOMPARE(error.value("data").toObject().value("uri").toString(), missing);
        }
        const QJsonObject live = server.modernRequest("resources/read", {{"uri", "omabook://library/books"}}).value("result").toObject();
        QCOMPARE(live.value("ttlMs").toInt(-1), 0);
        QCOMPARE(live.value("cacheScope").toString(), QString("private"));

        const QJsonArray prompts = server.request("prompts/list").value("result").toObject().value("prompts").toArray();
        QCOMPARE(prompts.size(), 2);
        const QJsonObject prompt = server.request("prompts/get", {{"name", "fix_details"}, {"arguments", QJsonObject{{"book", "Dune"}}}})
                                       .value("result").toObject();
        const QJsonObject message = prompt.value("messages").toArray().first().toObject();
        QCOMPARE(message.value("role").toString(), QString("user"));
        QVERIFY(message.value("content").toObject().value("text").toString().contains("Dune"));
        QCOMPARE(server.request("prompts/get", {{"name", "fix_details"}}).value("error").toObject().value("code").toInt(), -32602);
        QCOMPARE(server.request("prompts/get", {{"name", "nope"}}).value("error").toObject().value("code").toInt(), -32602);
        QVERIFY2(server.noise().isEmpty(), qPrintable(server.noise().join('\n')));
    }

    void theInterfaceIsChangedWithItsChecksAndUndone() {
        const Machine machine;
        Server server(machine);
        const QString folder = machine.dir.filePath("home/.config/omabook");
        QJsonObject now = server.data("customize_get");
        QCOMPARE(now.value("folder").toString(), folder);
        QVERIFY(now.value("keys").isNull());
        QVERIFY(now.value("commands").toArray().size() > 10);
        QVERIFY(QFile::exists(now.value("guide").toString()));
        QCOMPARE(now.value("can_undo").toBool(), false);

        // A change that checks out is made, in the files the window follows.
        now = server.data("customize_set", {{"keys", QJsonObject{{"library", QJsonObject{{"R", "read"}}}}},
                                            {"style", QJsonObject{{"radius", 8}}},
                                            {"settings", QJsonObject{{"font_size", 22}}}});
        QCOMPARE(now.value("keys").toObject().value("library").toObject().value("R").toString(), QString("read"));
        QCOMPARE(now.value("settings").toObject().value("font_size").toInt(), 22);
        QCOMPARE(now.value("can_undo").toBool(), true);
        QVERIFY(QFile::exists(folder + "/keys.json") && QFile::exists(folder + "/style.json"));

        // One that doesn't is refused whole, with what was there left in place.
        const QString refused = server.failure("customize_set", {{"keys", QJsonObject{{"library", QJsonObject{{"R", "fly"}}}}},
                                                                 {"settings", QJsonObject{{"font_size", 30}}}});
        QVERIFY2(refused.contains("Nothing was changed") && refused.contains("fly"), qPrintable(refused));
        QVERIFY(server.failure("customize_set", {{"settings", QJsonObject{{"font_size", 900}}}}).contains("font_size"));
        QVERIFY(server.failure("customize_set").contains("at least one"));
        now = server.data("customize_get");
        QCOMPARE(now.value("keys").toObject().value("library").toObject().value("R").toString(), QString("read"));
        QCOMPARE(now.value("settings").toObject().value("font_size").toInt(), 22);
        QVERIFY(now.value("problems").toObject().isEmpty());

        // The AI is still off: checking a change must not rewrite the rest of the settings.
        QCOMPARE(server.data("omabook_info").value("ai").toObject().value("source").toString(), QString("none"));
        QVERIFY2(server.noise().isEmpty(), qPrintable(server.noise().join('\n')));
    }

    void pluginsAndLibrariesAreListedAndSwitched() {
        const Machine machine;
        // No Calibre on this machine of the test's: a stand-in that knows no plugins.
        machine.program("calibre-debug", "exit 1\n");
        Server server(machine);
        QJsonArray plugins = server.data("plugins_list", {{"kind", "metadata"}}).value("plugins").toArray();
        QVERIFY(!plugins.isEmpty());
        auto enabled = [&](const QString &id) {
            for (const QJsonValue &plugin : server.data("plugins_list").value("plugins").toArray())
                if (plugin.toObject().value("id").toString() == id)
                    return plugin.toObject().value("enabled").toBool();
            return false;
        };
        QVERIFY(enabled("builtin:Crossref"));
        QCOMPARE(server.data("plugin_set_enabled", {{"id", "builtin:Crossref"}, {"enabled", false}}).value("enabled").toBool(), false);
        QVERIFY(!enabled("builtin:Crossref"));
        QVERIFY(server.failure("plugin_set_enabled", {{"id", "builtin:Nope"}, {"enabled", true}}).contains("no plugin"));

        const QJsonObject libraries = server.data("library_list");
        QCOMPARE(libraries.value("current").toString(), machine.library);
        QVERIFY(server.data("sync_status").contains("configured"));
        QCOMPARE(server.data("sync_status").value("configured").toBool(), false);
        QVERIFY(server.failure("sync_run").contains("No computer"));
        QVERIFY2(server.noise().isEmpty(), qPrintable(server.noise().join('\n')));
    }

    void theLibraryInUseIsFollowedAndSwitched() {
        // Without --library the server works on the library the settings
        // name, and follows it when that changes under it.
        const Machine machine;
        QProcess process;
        process.setProcessEnvironment(machine.environment());
        process.start(OMABOOK_BINARY, {"mcp"});
        QVERIFY(process.waitForStarted());
        int id = 0;
        auto data = [&](const QString &tool, const QJsonObject &arguments = {}) {
            process.write(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", ++id}, {"method", "tools/call"},
                                                    {"params", QJsonObject{{"name", tool}, {"arguments", arguments}}}})
                              .toJson(QJsonDocument::Compact) + "\n");
            while (!process.canReadLine())
                if (!process.waitForReadyRead(30000))
                    return QJsonObject();
            return QJsonDocument::fromJson(process.readLine()).object().value("result").toObject();
        };
        const QString first = machine.dir.filePath("home/Books"), second = machine.dir.filePath("Second");
        QCOMPARE(data("library_list").value("structuredContent").toObject().value("current").toString(), first);
        // A folder that is no library is not made one by a slip.
        QJsonObject result = data("library_switch", {{"folder", second}});
        QVERIFY(result.value("isError").toBool());
        QVERIFY(!QFile::exists(second));
        result = data("library_switch", {{"folder", second}, {"create", true}});
        QVERIFY2(!result.value("isError").toBool(), qPrintable(QJsonDocument(result).toJson()));
        QVERIFY(QFile::exists(second + "/omabook.db"));
        QVERIFY(Fixtures::write(machine.dir.filePath("Moved - Ann.txt"), "Words.\n"));
        QVERIFY(!data("book_add", {{"paths", QJsonArray{machine.dir.filePath("Moved - Ann.txt")}}}).value("isError").toBool());
        {
            Library opened(second);
            QVERIFY(opened.open());
            QCOMPARE(opened.books().size(), 1);
        }
        QCOMPARE(data("library_list").value("structuredContent").toObject().value("libraries").toArray().size(), 2);
        process.closeWriteChannel();
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitCode(), 0);
        // Nothing was said on standard error that points at a fault.
        const QString errors = QString::fromUtf8(process.readAllStandardError());
        QVERIFY2(!errors.contains("QSqlDatabase") && !errors.contains("QObject"), qPrintable(errors));
    }

    void installingRegistersWithAHarnessAndNothingElse() {
        const Machine machine;
        machine.program("claude");
        machine.program("codex");
        const QString binary = OMABOOK_BINARY;

        // --print changes nothing and runs nothing.
        Machine::Ran ran = machine.omabook({"mcp", "install", "claude", "--print"});
        QCOMPARE(ran.code, 0);
        QVERIFY2(ran.output.contains("claude mcp add --scope user omabook -- " + binary + " mcp"), qPrintable(ran.output));
        QVERIFY(ran.output.contains("\"mcpServers\""));
        QVERIFY(machine.ran("claude").isEmpty());

        // Installing runs the harness's own command, and says exactly that.
        ran = machine.omabook({"mcp", "install", "claude"});
        QCOMPARE(ran.code, 0);
        QCOMPARE(machine.ran("claude"), "mcp add --scope user omabook -- " + binary + " mcp");
        QVERIFY2(ran.output.contains("Running: claude mcp add --scope user omabook"), qPrintable(ran.output));
        QVERIFY(ran.output.contains("Registered") && ran.output.contains("claude mcp remove omabook"));

        // Without a name it is Omarchy's default agent.
        ran = machine.omabook({"mcp", "install"});
        QCOMPARE(ran.code, 1);
        QVERIFY2(ran.errors.contains("omarchy default agent"), qPrintable(ran.errors));
        QVERIFY(QDir().mkpath(machine.dir.filePath("home/.config/omarchy/defaults")));
        QVERIFY(Fixtures::write(machine.dir.filePath("home/.config/omarchy/defaults/agent"), "codex\n"));
        ran = machine.omabook({"mcp", "install"});
        QCOMPARE(ran.code, 0);
        QCOMPARE(machine.ran("codex"), "mcp add omabook -- " + binary + " mcp");
        QVERIFY(ran.output.contains("Omarchy's default agent"));

        // A harness that refuses is reported, with what it said.
        machine.program("pi", "echo 'server already exists' >&2\nexit 3\n");
        ran = machine.omabook({"mcp", "install", "pi"});
        QCOMPARE(ran.code, 1);
        QVERIFY(ran.output.contains("server already exists"));
        QVERIFY2(ran.errors.contains("did not register"), qPrintable(ran.errors));
        // One that isn't installed, and one Omabook knows no command for.
        ran = machine.omabook({"mcp", "install", "crush"});
        QCOMPARE(ran.code, 1);
        QVERIFY2(ran.errors.contains("Crush") && ran.errors.contains("\"mcpServers\""), qPrintable(ran.errors));

        // Nothing was written into the home but what Omabook itself keeps.
        const QStringList inHome = QDir(machine.dir.filePath("home")).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
        QCOMPARE(inHome, QStringList{".config"});
        QVERIFY(!QFile::exists(machine.dir.filePath("home/.config/omabook/AGENTS.md")));
    }

    void theModelIsChosenForOneRunOrOneCall() {
        // A stand-in Claude Code that writes down how it was run and answers.
        const Machine machine;
        machine.program("claude", "cat > /dev/null\necho 'An answer.'\n");
        QVERIFY(Fixtures::write(machine.dir.filePath("home/.config/omabook/config.json"),
                                "{\"ai_provider\": \"claude\", \"ai_models\": {\"claude\": {\"main\": \"opus\"}}}"));
        QVERIFY(Fixtures::write(machine.dir.filePath("Story - Ann.txt"), "It was a dark night.\n"));
        const QString story = machine.dir.filePath("Story - Ann.txt");
        auto lastRun = [&] { return machine.ran("claude").split('\n').last(); };

        // `omabook info --json` says both models.
        const QJsonObject ai = QJsonDocument::fromJson(machine.omabook({"info", "--json"}).output.toUtf8()).object().value("ai").toObject();
        QCOMPARE(ai.value("main_model").toString(), QString("opus"));
        QCOMPARE(ai.value("quick_model").toString(), QString("haiku"));
        QVERIFY(machine.omabook({"info"}).output.contains("for reading and parsing: haiku"));

        // A question is for the main model; --model overrules it for the run.
        Machine::Ran ran = machine.omabook({"ask", story, "Who narrates?", "--json"});
        QCOMPARE(ran.code, 0);
        QVERIFY2(lastRun().contains("--model opus"), qPrintable(lastRun()));
        QVERIFY(machine.omabook({"ask", story, "Who narrates?", "--model", "quick"}).output.contains("An answer."));
        QVERIFY2(lastRun().contains("--model haiku"), qPrintable(lastRun()));
        QCOMPARE(machine.omabook({"ask", "--model=sonnet", story, "Who narrates?"}).code, 0);
        QVERIFY2(lastRun().contains("--model sonnet"), qPrintable(lastRun()));

        // The tools that ask take a model too, for that call only.
        Server server(machine);
        QCOMPARE(server.data("ask_book", {{"path", story}, {"question", "Who?"}}).value("answer").toString(), QString("An answer."));
        QVERIFY2(lastRun().contains("--model opus"), qPrintable(lastRun()));
        QVERIFY(server.data("ask_book", {{"path", story}, {"question", "Who?"}, {"model", "quick"}}).contains("answer"));
        QVERIFY2(lastRun().contains("--model haiku"), qPrintable(lastRun()));
        QVERIFY(server.data("ask_library", {{"question", "What is here?"}, {"model", "fable"}}).contains("answer"));
        QVERIFY2(lastRun().contains("--model fable"), qPrintable(lastRun()));
        QVERIFY(server.data("ask_library", {{"question", "What is here?"}}).contains("answer"));
        QVERIFY2(lastRun().contains("--model opus"), qPrintable(lastRun()));
        // Finding series is reading into structure: the quick model's, unasked.
        QVERIFY(server.data("tidy_propose", {{"ai", true}}).contains("proposals"));
        QVERIFY2(lastRun().contains("--model haiku"), qPrintable(lastRun()));
        QCOMPARE(server.data("omabook_info").value("ai").toObject().value("quick_model").toString(), QString("haiku"));
        QVERIFY2(server.noise().isEmpty(), qPrintable(server.noise().join('\n')));
    }

    void theCommandLineSaysWhichAgentAnswers() {
        // `omabook info --json`: automatic is Omarchy's default agent.
        const Machine machine;
        machine.program("codex");
        machine.program("claude");
        QVERIFY(Fixtures::write(machine.dir.filePath("home/.config/omabook/config.json"), "{}"));
        QVERIFY(QDir().mkpath(machine.dir.filePath("home/.config/omarchy/defaults")));
        QVERIFY(Fixtures::write(machine.dir.filePath("home/.config/omarchy/defaults/agent"), "codex\n"));
        Machine::Ran ran = machine.omabook({"info", "--json"});
        QCOMPARE(ran.code, 0);
        QJsonObject ai = QJsonDocument::fromJson(ran.output.toUtf8()).object().value("ai").toObject();
        QCOMPARE(ai.value("provider").toString(), QString("codex"));
        QCOMPARE(ai.value("name").toString(), QString("Codex"));
        QCOMPARE(ai.value("source").toString(), QString("omarchy"));
        QCOMPARE(ai.value("omarchy_default").toString(), QString("codex"));

        QVERIFY(Fixtures::write(machine.dir.filePath("home/.config/omarchy/defaults/agent"), "crush\n"));
        ai = QJsonDocument::fromJson(machine.omabook({"info", "--json"}).output.toUtf8()).object().value("ai").toObject();
        QCOMPARE(ai.value("provider").toString(), QString("claude"));
        QCOMPARE(ai.value("source").toString(), QString("fallback"));
        QVERIFY(ai.value("problem").toString().contains("Crush"));
        QVERIFY(machine.omabook({"info"}).output.contains("Claude Code"));
        // The tools are listed without a server running.
        QVERIFY(machine.omabook({"mcp", "--tools"}).output.contains("books_search\t"));
    }

    void anAgentPicksATheme() {
        // A home with neither Omarchy nor Aether: automatic is Tokyo Night.
        const Machine machine;
        Server server(machine);
        QJsonObject listed = server.data("themes_list");
        QCOMPARE(listed.value("choice").toString(), QString("auto"));
        const QJsonArray themes = listed.value("themes").toArray();
        QVERIFY(themes.size() >= 24);
        QCOMPARE(themes.first().toObject().value("source").toObject().value("id").toString(), QString("tokyo-night"));
        QVERIFY(themes.first().toObject().value("current").toBool());

        const QJsonObject used = server.data("theme_use", {{"theme", "Catppuccin Latte"}});
        QCOMPARE(used.value("id").toString(), QString("catppuccin-latte"));
        QFile style(machine.dir.filePath("home/.config/omabook/style.json"));
        QVERIFY(style.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(style.readAll()).object().value("theme").toString(), QString("catppuccin-latte"));
        QVERIFY(server.failure("theme_use", {{"theme", "no-such-theme"}}).contains("no theme called"));
        listed = server.data("themes_list");
        QCOMPARE(listed.value("choice").toString(), QString("catppuccin-latte"));

        // `omabook info --json` says which theme and why.
        const QJsonObject theme = QJsonDocument::fromJson(machine.omabook({"info", "--json"}).output.toUtf8()).object().value("theme").toObject();
        QCOMPARE(theme.value("name").toString(), QString("Catppuccin Latte"));
        QCOMPARE(theme.value("kind").toString(), QString("bundled"));
        QVERIFY(theme.value("why").toString().startsWith("Chosen"));
        QVERIFY2(server.noise().isEmpty(), qPrintable(server.noise().join('\n')));
    }

    void anAgentSetsHowBooksAreSet() {
        const Machine machine;
        QVERIFY(Fixtures::epub(machine.dir.filePath("set.epub"), "The Set Book"));
        Server server(machine);
        const qint64 id = server.data("book_add", {{"paths", QJsonArray{machine.dir.filePath("set.epub")}}})
                              .value("added").toArray().first().toObject().value("id").toInteger();
        QVERIFY(id > 0);
        // The defaults, with what each may be.
        QJsonObject format = server.data("reading_format_get");
        QCOMPARE(format.value("effective").toObject().value("align").toString(), QString("start"));
        QCOMPARE(format.value("bounds").toObject().value("line_height").toObject().value("high").toInt(), 300);
        QVERIFY(!format.contains("own"));
        // "More space between the lines", for every book.
        format = server.data("reading_format_set", {{"line_height", 190}, {"align", "justify"}});
        QCOMPARE(format.value("defaults").toObject().value("line_height").toInt(), 190);
        // One book centred and larger; the rest of it is the defaults'.
        format = server.data("reading_format_set", {{"book", id}, {"align", "center"}, {"font_size", 24}});
        QCOMPARE(format.value("own").toObject(), (QJsonObject{{"align", "center"}, {"font_size", 24}}));
        QCOMPARE(format.value("effective").toObject().value("line_height").toInt(), 190);
        QCOMPARE(format.value("effective").toObject().value("align").toString(), QString("center"));
        QCOMPARE(server.data("reading_format_get", {{"book", id}}).value("own").toObject().value("font_size").toInt(), 24);
        QCOMPARE(server.data("reading_format_get").value("effective").toObject().value("align").toString(), QString("justify"));
        // Out of bounds is refused with the range, and nothing changes.
        const QString refused = server.failure("reading_format_set", {{"book", id}, {"margins", 500}, {"indent", 8}});
        QVERIFY2(refused.contains("0 to 160"), qPrintable(refused));
        QVERIFY(!server.data("reading_format_get", {{"book", id}}).value("own").toObject().contains("indent"));
        QVERIFY(server.failure("reading_format_set", {{"book", id}}).contains("at least one"));
        // Reset, the book follows the defaults again.
        format = server.data("reading_format_set", {{"book", id}, {"reset", true}});
        QVERIFY(format.value("own").toObject().isEmpty());
        QCOMPARE(format.value("effective").toObject().value("align").toString(), QString("justify"));
        // The longest line in characters, and the pages side by side.
        QCOMPARE(server.data("reading_format_get").value("bounds").toObject().value("measure").toObject().value("default").toInt(), 66);
        format = server.data("reading_format_set", {{"measure", 60}, {"pages", "2"}});
        QCOMPARE(format.value("defaults").toObject().value("measure").toInt(), 60);
        QCOMPARE(format.value("pages").toInt(), 2);
        QCOMPARE(server.data("reading_format_set", {{"pages", "auto"}}).value("pages").toString(), QString("auto"));
        QVERIFY(server.failure("reading_format_set", {{"measure", 30}}).contains("40 to 90"));
        QVERIFY(server.failure("reading_format_set", {{"book", id}, {"pages", "3"}}).contains("every book"));
        QVERIFY2(server.noise().isEmpty(), qPrintable(server.noise().join('\n')));
    }
    // An agent keeps collections, nested and named as the user likes, and
    // searches, asks and suggests within one.
    void anAgentKeepsCollections() {
        const Machine machine;
        QVERIFY(Fixtures::epub(machine.dir.filePath("pipes.epub"), "Pipe Networks"));
        QVERIFY(Fixtures::epub(machine.dir.filePath("sea.epub"), "The Sea Wall"));
        Server server(machine);
        const QJsonArray added = server.data("book_add", {{"paths", QJsonArray{machine.dir.filePath("pipes.epub"),
                                                                                 machine.dir.filePath("sea.epub")}}})
                                     .value("added").toArray();
        const qint64 pipes = added.at(0).toObject().value("id").toInteger(), sea = added.at(1).toObject().value("id").toInteger();
        QVERIFY(pipes > 0 && sea > 0);
        server.data("collection_add", {{"name", "Werk/Hydraulica"}, {"rule", "title:pipe"}});
        server.data("collection_add", {{"name", "Plezier"}});
        server.data("collection_put", {{"name", "Plezier"}, {"books", QJsonArray{sea}}, {"how", "include"}});
        // The tree, read from the top.
        QStringList paths;
        for (const QJsonValue &value : server.data("collections_list").value("result").toArray())
            paths << value.toObject().value("path").toString();
        QCOMPARE(paths, (QStringList{"Werk", "Werk/Hydraulica", "Plezier"}));
        const QJsonObject werk = server.data("collection_get", {{"name", "Werk"}});
        QCOMPARE(werk.value("count").toInt(), 1);
        QCOMPARE(werk.value("books").toArray().first().toObject().value("id").toInteger(), pipes);
        // A search within one collection.
        QCOMPARE(server.data("books_search", {{"collection", "Plezier"}}).value("total").toInt(), 1);
        QVERIFY(server.failure("books_search", {{"collection", "Nowhere"}}).contains("Nowhere"));
        // Moved and renamed with a path, what is inside goes along.
        server.data("collection_rename", {{"name", "Werk"}, {"new_name", "Studie"}});
        QCOMPARE(server.data("collection_get", {{"name", "Studie/Hydraulica"}}).value("count").toInt(), 1);
        server.data("collection_move", {{"name", "Plezier"}, {"parent", "Studie"}});
        QCOMPARE(server.data("collection_get", {{"name", "Studie"}}).value("count").toInt(), 2);
        // Its own reading format.
        const QJsonObject format = server.data("collection_format", {{"name", "Studie"}, {"options", QJsonObject{{"measure", 80}}}});
        QCOMPARE(format.value("format").toObject().value("measure").toInt(), 80);
        server.data("collection_put", {{"name", "Studie/Plezier"}, {"books", QJsonArray{sea}}, {"how", "rule"}});
        QCOMPARE(server.data("collection_get", {{"name", "Studie/Plezier"}}).value("count").toInt(), 0);
        server.data("collection_remove", {{"name", "Studie/Hydraulica"}});
        QVERIFY(server.failure("collection_get", {{"name", "Studie/Hydraulica"}}).contains("no collection"));
        QVERIFY2(server.noise().isEmpty(), qPrintable(server.noise().join('\n')));
    }
};

QTEST_GUILESS_MAIN(McpTests)
#include "mcp_tests.moc"
