#include "scriptsession.h"

#include <QCommandLineParser>
#include <QFileInfo>
#include <algorithm>
#include <cstdio>
#include <print>
#include <utility>

#include "airegistry.h"
#include "numberoptions.h"
#include "pluginfilesync.h"
#include "scriptplugins.h"
#include "testaudio.h"

namespace {

void say(const QString &why) { std::println(stderr, "{}", qPrintable(why)); }

} // namespace

ScriptSession::ScriptSession(ScriptTools::Context context) : m_context(std::move(context)) {
    if (!m_context.settings) {
        m_ownSettings = std::make_unique<Settings>();
        m_context.settings = m_ownSettings.get();
    }
    if (m_context.folder.isEmpty())
        m_context.folder = ScriptPlugins::defaultFolder();
}

Library *ScriptSession::library() {
    if (m_context.library)
        return m_context.library();
    if (!m_ownLibrary && !m_libraryTried) {
        m_libraryTried = true;
        auto library = std::make_unique<Library>(m_context.settings->libraryPath());
        if (library->open())
            m_ownLibrary = std::move(library);
    }
    return m_ownLibrary.get();
}

ScriptServices ScriptSession::services() {
    ScriptServices services;
    services.settings = m_context.settings;
    services.library = [this] { return library(); };
    services.ai = AiRegistry::answerer(*m_context.settings);
    services.network = &m_network;
    services.audio = m_context.audio;
    services.microphoneGranted = m_context.microphoneGranted;
    // A command fetches a file that travels on demand while it waits (#135).
    services.fetchFile = [this](const QString &plugin, const QString &path, const std::function<void(bool)> &done) {
        const Library *library = this->library();
        const auto fetched = library ? PluginFileSync::fetch(*library, folder(), plugin, path, m_context.settings->syncTarget())
                                     : PluginFileSync::Fetched::Failed;
        done(fetched == PluginFileSync::Fetched::Here || fetched == PluginFileSync::Fetched::Fetched);
    };
    return services;
}

bool ScriptSession::scratch(const Settings &settings, const QString &folder) {
    // The same folder by any path to it, links followed.
    const auto same = [](const QString &one, const QString &other) {
        const QString first = QFileInfo(one).canonicalFilePath();
        return !first.isEmpty() && first == QFileInfo(other).canonicalFilePath();
    };
    if (folder.isEmpty() || same(folder, settings.libraryPath()))
        return false;
    const QStringList own = settings.libraries();
    return std::ranges::none_of(own, [&](const QString &library) { return same(folder, library); });
}

bool ScriptSession::asAsked(const QCommandLineParser &parser, ScriptTools::Context &context, std::unique_ptr<Library> &library,
                            std::optional<AudioDevices> &room) {
    if (const auto why = Numbers::problem(parser, {{.name = QStringLiteral("wait"), .isWhole = false}})) {
        say(*why);
        return false;
    }
    if (parser.isSet("library")) {
        library = std::make_unique<Library>(parser.value("library"));
        QString error;
        if (!library->open(&error)) {
            say(error);
            return false;
        }
        context.library = [&library] { return library.get(); };
    }
    const bool testing = parser.isSet("test-microphone");
    if (!testing && !parser.isSet("allow-microphone"))
        return true;
    if (!parser.isSet("library") || !ScriptSession::scratch(Settings(), parser.value("library"))) {
        say("--test-microphone and --allow-microphone are for a scratch library: name one with --library that is "
            "none of your own libraries, such as a new folder.");
        return false;
    }
    const auto hears = testing ? TestAudio::microphone(parser.value("test-microphone")) : std::nullopt;
    if (testing && !hears) {
        say("--test-microphone hears tone, silence, or a WAV file of mono 16-bit samples at 48 kHz.");
        return false;
    }
    if (hears)
        context.audio = &room.emplace(*hears, [] { return TestAudio::speakers(); });
    context.microphoneGranted = parser.isSet("allow-microphone");
    return true;
}
