// The window's side of the script plugins: starting them with what they
// are given to work with, and showing what they say, change and put on the
// shelf. Kept apart from the rest of Backend, which starts them once.

#include "backend.h"

void Backend::startScripts(const QString &pluginFolder) {
    // The script plugins start once there is a library for them to read.
    // What they say is said in the window, and a book they change shows.
    ScriptServices services;
    services.settings = m_settings;
    services.library = [this] { return m_library.get(); };
    services.ai = [this](QString *why) { return m_aiFactory(why); };
    services.network = &m_network;
    services.audio = &m_audio;
    services.desk = &m_pluginDesk;
    m_scripts = std::make_unique<ScriptPlugins>(services, pluginFolder);
    // The book formats scripts add are read like Omabook's own.
    m_scripts->provideFormats();
    connect(m_scripts.get(), &ScriptPlugins::said, this, &Backend::say);
    connect(m_scripts.get(), &ScriptPlugins::touchedLibrary, &m_reloadSoon, qOverload<>(&QTimer::start));
    connect(m_scripts.get(), &ScriptPlugins::changed, this, &Backend::pluginsChanged);
    // What plugins put on the shelf comes and goes with them.
    connect(m_scripts.get(), &ScriptPlugins::changed, this, [this] {
        m_model.setLaunchers(m_scripts->launchers());
        ++m_revision;
        emit revisionChanged();
    });
    // The plugin manager looks again whenever the plugins change, and says
    // so when it changed them itself.
    m_pluginManager = std::make_unique<PluginManager>(m_settings, &m_plugins, m_scripts.get(), m_picker, &m_network);
    connect(this, &Backend::pluginsChanged, m_pluginManager.get(), &PluginManager::refresh);
    connect(m_pluginManager.get(), &PluginManager::touched, this, &Backend::pluginsChanged);
    // A plugin that updated itself, or went back a version, says so once.
    connect(m_pluginManager.get(), &PluginManager::said, this, &Backend::say);
    if (m_library)
        m_scripts->libraryChanged(m_library->books(), libraryPath());
    // A plugin hears of its data that came from elsewhere: after the window's
    // own sync (which reloads), and whenever the database's files change, by a
    // sync from the command line too. The first look marks where the log is.
    m_scripts->lookAtTheLog();
    connect(this, &Backend::revisionChanged, m_scripts.get(), &ScriptPlugins::lookAtTheLog);
    connect(&m_reloadSoon, &QTimer::timeout, m_scripts.get(), &ScriptPlugins::lookAtTheLog);
    QTimer::singleShot(0, m_scripts.get(), [this] { m_scripts->emitEvent("app.started"); });
}
