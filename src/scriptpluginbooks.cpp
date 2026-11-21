// What of the plugins is about books (ScriptPlugins, scriptplugins.h): the
// formats they read, which go into Formats, the books that are a plugin's
// own (PluginClaims) and their words, for the search (BookWords, #136),
// a plugin's own kind of book opening in its view, in place of the reader
// (#125), and what the scripts are told of books and of where the reader is,
// with each book's global id (#137).

#include <algorithm>

#include "formatprovider.h"
#include "formats.h"
#include "pluginclaims.h"
#include "scriptplugin.h"
#include "scriptplugins.h"
#include "scriptreading.h"
#include "spaces.h"

QJsonObject ScriptPlugins::told(const Book &book) const { return ScriptBook::of(m_services.library ? m_services.library() : nullptr, book); }

namespace {

// What an event says, each book in it by the id that is the same on every
// computer too (#137): the one the reader's place is of, and one told of whole.
[[nodiscard]] QJsonObject withGlobalIds(const Library &library, const QString &event, QJsonObject out) {
    const qint64 id = out.value("id").toInteger();
    if (id > 0 && (event.startsWith("book.") || event.startsWith("page.")) && !out.contains("global_id")) {
        const QString known = out.value("book").toObject().value("global_id").toString();
        out.insert("global_id", known.isEmpty() ? Spaces::bookId(library, id) : known);
    }
    if (QJsonObject book = out.value("book").toObject(); !book.isEmpty() && !book.contains("global_id")) {
        book.insert("global_id", Spaces::bookId(library, book.value("id").toInteger()));
        out.insert("book", book);
    }
    return out;
}

} // namespace

QJsonObject ScriptPlugins::follow(const QString &event, const QJsonObject &payload) {
    const Library *library = m_services.library ? m_services.library() : nullptr;
    const QJsonObject out = library ? withGlobalIds(*library, event, payload) : payload;
    // What was selected was of the book before.
    if (event == "book.closed" || event == "book.opened")
        setSelection({});
    if (event == "book.closed") {
        m_reading = {};
    } else if (event == "book.opened" || event == "page.turned") {
        // Where the reader is, as the events say it: the same for all three.
        m_reading = out;
        m_reading.remove("book");
    }
    if (event.startsWith("book."))
        lookAtTheBook();
    return out;
}

void ScriptPlugins::setSelection(const QJsonObject &selection) {
    if (selection == m_selection)
        return;
    m_selection = selection;
    // None, of the book it was in.
    const QJsonObject said = !selection.isEmpty() ? selection
                                                  : QJsonObject{{"book", m_reading.value("id")},
                                                                {"global_id", m_reading.value("global_id")},
                                                                {"passage", QJsonValue::Null}};
    emitEvent("reader.selected", said);
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
    // And what the books of their formats say is read for the search, of
    // those that came or changed since it was last read (#136).
    if (library) {
        for (const auto &entry : m_plugins)
            entry.second->indexWords();
    }
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
