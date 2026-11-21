// What of the plugins is about books (ScriptPlugins, scriptplugins.h): the
// formats they read, which go into Formats, the books that are a plugin's
// own (PluginClaims), and a plugin's own kind of book opening in its view,
// in place of the reader (#125).

#include <algorithm>

#include "formatprovider.h"
#include "formats.h"
#include "pluginclaims.h"
#include "scriptplugin.h"
#include "scriptplugins.h"

QJsonObject ScriptPlugins::told(const Book &book) {
    QJsonObject object = book.toJson();
    object.insert("has_cover", book.hasCover);
    object.insert("identifiers", book.identifiers);
    object.insert("generated", book.identifiers.value("generated").toString());
    return object;
}

// Formats holds what the running plugins register, no more and no less: a
// plugin loaded again brings its formats anew, one that is gone takes them.
// What one installed here that isn't running says it reads is known as
// that, so a book in it says to switch it on.
void ScriptPlugins::syncFormats() {
    if (!m_providesFormats)
        return;
    QStringList wanted;
    for (const auto &entry : m_plugins) {
        for (const ScriptPlugin::Format &format : entry.second->formats()) {
            const FormatProvider provider = entry.second->provider(format);
            Formats::addProvider(provider);
            wanted << provider.id;
        }
    }
    for (const QString &id : std::as_const(m_formatIds))
        if (!wanted.contains(id))
            Formats::removeProvider(id);
    m_formatIds = wanted;
    QList<PluginFormat> idle;
    for (const Info &info : list())
        if (!info.loaded)
            idle << info.manifest.formats;
    Formats::setInstalledPluginFormats(idle);
}

// The books that came without their plugin's mark (by sync) are its own
// again, once it is running: when it loads, and when they arrive.
bool ScriptPlugins::claimBooks(const QList<Book> &books) {
    Library *library = m_follow && m_services.library ? m_services.library() : nullptr;
    PluginClaims::Registered registered;
    for (const auto &[id, plugin] : m_plugins)
        for (const ScriptPlugin::Format &format : plugin->formats())
            registered[id] << format.extensions;
    if (!library || registered.empty() || PluginClaims::claim(*library, books.isEmpty() ? library->books() : books, registered).isEmpty())
        return false;
    emit touchedLibrary();
    return true;
}

QList<Book> ScriptPlugins::claimed(const QList<Book> &books) {
    const Library *library = m_services.library ? m_services.library() : nullptr;
    return claimBooks(books) && library ? library->books() : books;
}

bool ScriptPlugins::openInView(const Book &book) {
    // A book with a file Omabook reads or converts itself is read, whatever a plugin says.
    if (book.formats.isEmpty() || std::ranges::any_of(book.formats, Formats::ownFormat))
        return false;
    for (const auto &[id, plugin] : m_plugins) {
        const auto hasView = [&plugin](const QString &view) {
            return std::ranges::any_of(plugin->panels(), [&view](const ScriptPlugin::Panel &panel) { return panel.view == view; });
        };
        for (const ScriptPlugin::Format &format : plugin->formats()) {
            const bool its =
                std::ranges::any_of(book.formats, [&format](const QString &extension) { return format.extensions.contains(extension); });
            if (its && !format.view.isEmpty() && hasView(format.view)) {
                emit viewRequested(id, format.view, QVariantMap{{"book", told(book).toVariantMap()}}, false);
                return true;
            }
        }
    }
    return false;
}
