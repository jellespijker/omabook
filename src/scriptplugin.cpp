#include "scriptplugin.h"

#include "log.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMetaMethod>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextDocument>
#include <QTimer>
#include <QUrl>
#include <algorithm>

#include "ai.h"
#include "formats.h"
#include "graphlinks.h"
#include "graphtools.h"
#include "library.h"
#include "scriptapi.h"
#include "scriptform.h"
#include "settings.h"

namespace {

// The name the prelude is run under, so its own frames can be told from the
// script's when an error says where it happened.
constexpr auto preludeName = "omabook-api";

QString compact(const QJsonValue &value) {
    // A document is an object or an array, so anything else rides in one.
    const QByteArray wrapped = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(wrapped.mid(1, wrapped.size() - 2));
}

QJsonValue parsed(const QString &json) {
    return QJsonDocument::fromJson(("[" + json + "]").toUtf8()).array().at(0);
}

QStringList stringList(const QJsonValue &value) {
    QStringList list;
    for (const QJsonValue &item : value.toArray())
        list << item.toString();
    return list;
}

QString htmlFromMarkdown(const QString &markdown) {
    QTextDocument converted;
    converted.setMarkdown(markdown);
    return converted.toHtml();
}

// Plain text as paragraphs: a blank line parts them, and a line's end is kept.
QString htmlFromText(const QString &text) {
    QString html;
    static const QRegularExpression blank("\\n[ \\t]*\\n");
    for (const QString &paragraph : QString(text).replace("\r\n", "\n").split(blank, Qt::SkipEmptyParts))
        if (!paragraph.trimmed().isEmpty())
            html += "<p>" + paragraph.trimmed().toHtmlEscaped().replace('\n', "<br>") + "</p>\n";
    return html;
}

QJsonObject bookJson(const Library &library, const Book &book, bool whole) {
    QJsonObject object = book.toJson();
    object.insert("has_cover", book.hasCover);
    // Always there, so a script can tell its own books without looking twice.
    object.insert("identifiers", book.identifiers);
    object.insert("generated", book.identifiers.value("generated").toString());
    if (whole) {
        QJsonObject files;
        for (const QString &format : book.formats)
            files.insert(format, library.formatPath(book.id, format));
        object.insert("files", files);
        object.insert("cover", library.coverPath(book.id));
    }
    return object;
}

} // namespace

ScriptManifest ScriptManifest::read(const QString &folder) {
    ScriptManifest manifest;
    manifest.folder = QDir(folder).absolutePath();
    manifest.id = QFileInfo(manifest.folder).fileName();
    manifest.name = manifest.id;
    QFile file(manifest.folder + "/plugin.json");
    if (!file.open(QIODevice::ReadOnly)) {
        manifest.problem = "There is no plugin.json in " + manifest.folder + ".";
        return manifest;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    const QJsonObject object = document.object();
    manifest.name = object.value("name").toString(manifest.id);
    manifest.version = object.value("version").toString();
    manifest.author = object.value("author").toString();
    manifest.description = object.value("description").toString();
    manifest.script = object.value("script").toString();
    manifest.permissions = stringList(object.value("permissions"));
    if (error.error != QJsonParseError::NoError) {
        manifest.problem = QString("plugin.json isn't valid JSON: %1 (at character %2).").arg(error.errorString()).arg(error.offset);
        return manifest;
    }
    if (!document.isObject()) {
        manifest.problem = "plugin.json should be a JSON object.";
        return manifest;
    }
    // One written for a newer Omabook is not loaded: what it calls may not be here.
    bool malformed = false;
    const QString need = ScriptApi::needsNewer(object.value("omabook"), &malformed);
    if (!need.isEmpty()) {
        manifest.problem = need;
        manifest.needsNewer = !malformed;
        return manifest;
    }
    if (!manifest.isScript())
        return manifest;
    for (const QString &permission : std::as_const(manifest.permissions))
        if (!ScriptApi::permissionNames().contains(permission))
            manifest.problem = "plugin.json asks for the permission \"" + permission + "\", which there isn't; there are: "
                               + ScriptApi::permissionNames().join(", ") + ".";
    // The script is a file of the plugin's own folder, not a way out of it.
    const QString script = QDir::cleanPath(manifest.folder + "/" + manifest.script);
    if (!script.startsWith(manifest.folder + "/") || !QFileInfo(script).isFile())
        manifest.problem = "plugin.json names the script \"" + manifest.script + "\", and there is no such file in the plugin's folder.";
    return manifest;
}

ScriptWatchdog::ScriptWatchdog(QJSEngine *engine, std::function<void()> fired)
    : m_engine(engine), m_fired(std::move(fired)), m_thread([this] { watch(); }) {}

ScriptWatchdog::~ScriptWatchdog() {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_quit = true;
    }
    m_wake.notify_all();
    m_thread.join();
}

void ScriptWatchdog::arm(std::chrono::milliseconds limit) {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_armed = true;
        m_deadline = std::chrono::steady_clock::now() + limit;
    }
    m_wake.notify_all();
}

void ScriptWatchdog::disarm() {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_armed = false;
    }
    m_wake.notify_all();
}

void ScriptWatchdog::watch() {
    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_quit) {
        if (!m_armed) {
            m_wake.wait(lock);
            continue;
        }
        // Woken early means disarmed, armed anew, or quitting: look again.
        if (m_wake.wait_until(lock, m_deadline) != std::cv_status::timeout || !m_armed
            || std::chrono::steady_clock::now() < m_deadline)
            continue;
        m_armed = false;
        m_engine->setInterrupted(true);
        m_fired();
    }
}

// The C++ side of the `omabook` object: what scriptprelude.js builds it
// from. Objects cross as JSON text, which keeps what a script can hand over
// to what JSON can hold. A permission is checked here as well as there.
class ScriptHost : public QObject {
    Q_OBJECT

public:
    explicit ScriptHost(ScriptPlugin *plugin) : p(plugin) {}

    Q_INVOKABLE void enter() { p->enter(); }
    Q_INVOKABLE void leave() { p->leave(); }
    Q_INVOKABLE void caught(const QJSValue &error) { p->m_log << "warn: " + p->describe(error); }
    Q_INVOKABLE void settled(int token, bool ok, const QString &json, const QJSValue &error) {
        const ScriptPlugin::Done done = p->m_waiting.take(token);
        if (done)
            done(ok ? parsed(json) : QJsonValue(), ok ? QString() : p->describe(error));
        p->busy(-1);
    }

    Q_INVOKABLE void say(const QString &text) { emit p->said(text); }
    Q_INVOKABLE void log(const QString &level, const QString &text) {
        p->m_log << (level == "log" ? text : level + ": " + text);
        // A plugin's own words go in Omabook's log too, under its name.
        if (level == "error" || level == "warn")
            qCWarning(lcPlugins).noquote() << p->manifest().id + ":" << text;
        else
            qCInfo(lcPlugins).noquote() << p->manifest().id + ":" << text;
    }

    Q_INVOKABLE void on(const QString &event, const QJSValue &handler) {
        p->m_handlers[event] << handler;
        emit p->registered();
    }
    Q_INVOKABLE void off(const QString &event, const QJSValue &handler) {
        QList<QJSValue> &handlers = p->m_handlers[event];
        for (qsizetype i = handlers.size() - 1; i >= 0; --i)
            if (handlers.at(i).strictlyEquals(handler))
                handlers.removeAt(i);
        emit p->registered();
    }
    Q_INVOKABLE void after(double milliseconds, const QJSValue &function) {
        p->busy(1);
        QTimer::singleShot(std::clamp(int(milliseconds), 0, 24 * 60 * 60 * 1000), p, [this, function] {
            QString problem;
            p->call(function, {}, &problem);
            if (!problem.isEmpty())
                p->report(problem);
            p->busy(-1);
        });
    }
    Q_INVOKABLE QJSValue include(const QString &name) {
        const QString path = p->inside(name, false);
        QFile file(path);
        if (path.isEmpty() || !file.open(QIODevice::ReadOnly)) {
            p->m_engine.throwError("There is no script \"" + name + "\" in the plugin's folder.");
            return {};
        }
        const QJSValue result = p->m_engine.evaluate(QString::fromUtf8(file.readAll()), name);
        if (result.isError())
            p->m_engine.throwError(result);
        return result;
    }

    Q_INVOKABLE void registerMetadata(const QString &name, const QJSValue &fields, const QJSValue &search) {
        QStringList keys;
        for (int i = 0; i < fields.property("length").toInt(); ++i)
            keys << fields.property(i).toString();
        for (qsizetype i = p->m_sources.size() - 1; i >= 0; --i)
            if (p->m_sources.at(i).name == name)
                p->m_sources.removeAt(i);
        p->m_sources.append({name, keys, search});
        emit p->registered();
    }
    Q_INVOKABLE void registerCatalog(const QString &format, const QString &description, const QJSValue &make) {
        for (qsizetype i = p->m_catalogs.size() - 1; i >= 0; --i)
            if (p->m_catalogs.at(i).format == format)
                p->m_catalogs.removeAt(i);
        p->m_catalogs.append({format.toLower(), description, make});
        emit p->registered();
    }
    Q_INVOKABLE void registerImporter(const QString &name, const QJSValue &added) {
        for (qsizetype i = p->m_importers.size() - 1; i >= 0; --i)
            if (p->m_importers.at(i).name == name)
                p->m_importers.removeAt(i);
        p->m_importers.append({name, added});
        emit p->registered();
    }
    Q_INVOKABLE void registerCommand(const QString &id, const QString &title, const QString &place, const QString &key,
                                     const QString &when, const QJSValue &run) {
        const QString whole = place + "." + p->m_manifest.id + ":" + id;
        for (qsizetype i = p->m_commands.size() - 1; i >= 0; --i)
            if (p->m_commands.at(i).id == whole)
                p->m_commands.removeAt(i);
        p->m_commands.append({whole, title, place, key, run, when, true});
        emit p->registered();
    }
    Q_INVOKABLE void setCommandEnabled(const QString &id, bool enabled) {
        for (ScriptPlugin::Command &command : p->m_commands) {
            if (command.id == id || command.id.section(':', -1) == id) {
                command.enabled = enabled;
                emit p->registered();
                return;
            }
        }
        p->m_engine.throwError("There is no command \"" + id + "\" of this plugin's to switch.");
    }
    Q_INVOKABLE void setPanelVisible(const QString &source, bool visible) {
        bool found = false;
        for (ScriptPlugin::Panel &panel : p->m_panels) {
            if (QFileInfo(panel.source).fileName() == QFileInfo(source).fileName()) {
                panel.visible = visible;
                found = true;
            }
        }
        if (found)
            emit p->registered();
        else
            p->m_engine.throwError("There is no panel \"" + source + "\" of this plugin's to show or hide.");
    }
    Q_INVOKABLE void showGraph(const QString &scope, double book) { emit p->graphRequested(scope, qint64(book)); }
    Q_INVOKABLE void hold(int change) { p->m_holds = qMax(0, p->m_holds + change); }
    Q_INVOKABLE void registerPanel(const QString &place, const QString &title, int width, const QString &source, const QString &when) {
        const QString path = p->inside(source, false);
        if (path.isEmpty() || !QFileInfo(path).isFile()) {
            p->m_engine.throwError("There is no \"" + source + "\" in the plugin's folder for the panel.");
            return;
        }
        // The same file again is the same panel, said anew.
        for (qsizetype i = p->m_panels.size() - 1; i >= 0; --i)
            if (p->m_panels.at(i).source == path)
                p->m_panels.removeAt(i);
        p->m_panels.append({place, title, path, std::clamp(width, 120, 900), when, true});
        emit p->registered();
    }
    Q_INVOKABLE void registerFormat(const QString &name, const QJSValue &extensions, const QJSValue &claims, const QJSValue &open) {
        QStringList list;
        for (int i = 0; i < extensions.property("length").toInt(); ++i)
            list << extensions.property(i).toString();
        for (qsizetype i = p->m_formats.size() - 1; i >= 0; --i)
            if (p->m_formats.at(i).name == name)
                p->m_formats.removeAt(i);
        p->m_formats.append({name, list, claims, open});
        emit p->registered();
    }
    Q_INVOKABLE void registerLauncher(const QString &id, const QString &title, const QString &author, const QString &description,
                                      const QString &cover, const QJSValue &open, const QJSValue &removed) {
        QString picture;
        if (!cover.isEmpty()) {
            picture = p->inside(cover, false);
            if (picture.isEmpty() || !QFileInfo(picture).isFile()) {
                p->m_engine.throwError("There is no \"" + cover + "\" in the plugin's folder for the launcher's cover.");
                return;
            }
        }
        // The same id again is the same launcher, said anew; a plugin has
        // few, since each is a place on everybody's shelf.
        for (qsizetype i = p->m_launchers.size() - 1; i >= 0; --i)
            if (p->m_launchers.at(i).id == id)
                p->m_launchers.removeAt(i);
        if (p->m_launchers.size() >= 3) {
            p->m_engine.throwError(QString("A plugin puts at most three launchers on the shelf."));
            return;
        }
        p->m_launchers.append({id, title.simplified(), author.simplified(), description.trimmed(), picture, open, removed});
        emit p->registered();
    }
    Q_INVOKABLE void removeLauncher(const QString &id) {
        const qsizetype before = p->m_launchers.size();
        p->m_launchers.removeIf([&id](const ScriptPlugin::Launcher &launcher) { return launcher.id == id; });
        if (p->m_launchers.size() != before)
            emit p->registered();
    }
    Q_INVOKABLE QString markdownToHtml(const QString &text) { return htmlFromMarkdown(text); }
    Q_INVOKABLE QString bookEntries(int token) {
        const BookSource *book = this->book(token);
        return compact(QJsonArray::fromStringList(book ? book->entries() : QStringList()));
    }
    Q_INVOKABLE QJSValue bookText(int token, const QString &name) {
        const BookSource *book = this->book(token);
        // A file that isn't there, or isn't in the book, is no text at all.
        return book && !book->read(name).isNull() ? QJSValue(book->text(name)) : QJSValue(QJSValue::NullValue);
    }
    Q_INVOKABLE QJSValue bookBytes(int token, const QString &name) {
        const BookSource *book = this->book(token);
        const QByteArray bytes = book ? book->read(name) : QByteArray();
        return bytes.isNull() ? QJSValue(QJSValue::NullValue) : QJSValue(QString::fromLatin1(bytes.toBase64()));
    }
    Q_INVOKABLE QString bookResolve(const QString &from, const QString &relative) { return BookSource::resolve(from, relative); }

    Q_INVOKABLE void uiSet(const QString &key, const QString &json) {
        p->m_data.insert(key, parsed(json));
        emit p->dataChanged();
    }

    Q_INVOKABLE QString libraryBooks(const QString &query) {
        QJsonArray books;
        if (const Library *library = this->library(false))
            for (const Book &book : library->books(query))
                books << bookJson(*library, book, false);
        return compact(books);
    }
    Q_INVOKABLE QString libraryBook(double id) {
        const Library *library = this->library(false);
        const auto book = library ? library->book(qint64(id)) : std::nullopt;
        return book ? compact(bookJson(*library, *book, true)) : QString("null");
    }
    Q_INVOKABLE QString librarySetMeta(double id, const QString &details) {
        Library *library = this->library(true);
        const auto book = this->book(library, id);
        if (!book)
            return "null";
        // Only what was given changes: the details are laid over the book's own.
        QJsonObject merged = book->meta.toJson();
        const QJsonObject given = parsed(details).toObject();
        for (auto it = given.begin(); it != given.end(); ++it)
            merged.insert(it.key(), it.value());
        if (merged.value("published").isDouble())
            merged.insert("published", QString::number(merged.value("published").toInt()));
        if (merged.value("authors").isString())
            merged.insert("authors", QJsonArray::fromStringList(BookMeta::splitAuthors(merged.value("authors").toString())));
        const BookMeta meta = BookMeta::fromJson(merged);
        QString error;
        if (meta.title.isEmpty())
            error = "A book needs a title.";
        if (!error.isEmpty() || !library->setMeta(book->id, meta, &error)) {
            p->m_engine.throwError(error);
            return "null";
        }
        emit p->touchedLibrary();
        return compact(bookJson(*library, *library->book(book->id), true));
    }
    Q_INVOKABLE void librarySetReading(double id, const QString &reading) {
        Library *library = this->library(true);
        const auto book = this->book(library, id);
        if (!book)
            return;
        const QJsonObject given = parsed(reading).toObject();
        const QString shelf = given.contains("shelf") ? given.value("shelf").toString() : book->shelf;
        if (!QStringList{"", "to-read", "reading", "read"}.contains(shelf)) {
            p->m_engine.throwError("A shelf is \"to-read\", \"reading\", \"read\" or \"\"; not \"" + shelf + "\".");
            return;
        }
        library->setReading(book->id, shelf, std::clamp(given.value("rating").toInt(book->rating), 0, 5),
                            given.contains("finished") ? given.value("finished").toString().simplified() : book->finished);
        emit p->touchedLibrary();
    }
    Q_INVOKABLE void librarySetIdentifier(double id, const QString &scheme, const QString &value) {
        Library *library = this->library(true);
        // Who made a book is said by create alone: a plugin that could call
        // any book its own could then rewrite it.
        if (scheme == "generated") {
            p->m_engine.throwError(QString("\"generated\" says which plugin made a book and is set by omabook.library.create alone."));
            return;
        }
        if (const auto book = this->book(library, id)) {
            library->setIdentifier(book->id, scheme, value);
            emit p->touchedLibrary();
        }
    }
    Q_INVOKABLE QString libraryCreate(const QString &details, const QString &name) {
        Library *library = this->library(true);
        const QString file = library ? dataFile(name) : QString();
        if (file.isEmpty())
            return "null";
        // What the script says of the book, over what the file says of itself.
        QJsonObject given = parsed(details).toObject();
        if (given.value("authors").isString())
            given.insert("authors", QJsonArray::fromStringList(BookMeta::splitAuthors(given.value("authors").toString())));
        BookMeta meta = Formats::readMeta(file);
        meta.apply(BookMeta::fromJson(given));
        if (given.contains("series_index"))
            meta.seriesIndex = given.value("series_index").toDouble();
        const Library::Added added = library->add(file, meta);
        if (!added.error.isEmpty()) {
            p->m_engine.throwError(added.error);
            return "null";
        }
        // The very same file may be in the library already. It is this
        // plugin's to hand back only if this plugin made it.
        if (added.duplicate && library->book(added.id)->identifiers.value("generated").toString() != p->m_manifest.id) {
            p->m_engine.throwError(QString("That very file is already in the library as book %1, which this plugin did not make.").arg(added.id));
            return "null";
        }
        library->setIdentifier(added.id, "generated", p->m_manifest.id);
        emit p->touchedLibrary();
        return compact(bookJson(*library, *library->book(added.id), true));
    }
    Q_INVOKABLE QString libraryReplaceFile(double id, const QString &name) {
        Library *library = this->library(true);
        const auto book = own(library, id, "rewritten");
        const QString file = book ? dataFile(name) : QString();
        if (file.isEmpty())
            return "null";
        // A new version of the file the book has, not another format beside it.
        if (!book->formats.contains(Formats::extension(file))) {
            p->m_engine.throwError(QString("Book %1 has no .%2 file to replace; it has: %3.")
                                       .arg(book->id).arg(Formats::extension(file), book->formats.join(", ")));
            return "null";
        }
        QString error;
        if (!library->addFormat(book->id, file, &error)) {
            p->m_engine.throwError(error);
            return "null";
        }
        emit p->touchedLibrary();
        // The reader follows a book that grew under it, asked or not.
        emit p->reloadBookRequested(book->id);
        return compact(bookJson(*library, *library->book(book->id), true));
    }
    Q_INVOKABLE bool libraryRemove(double id) {
        Library *library = this->library(true);
        const auto book = own(library, id, "removed");
        QString error;
        if (!book)
            return false;
        if (!library->remove(book->id, &error)) {
            p->m_engine.throwError(error);
            return false;
        }
        emit p->touchedLibrary();
        return true;
    }
    Q_INVOKABLE double libraryAddEntry(const QString &details, bool paper) {
        Library *library = this->library(true);
        if (!library)
            return 0;
        const Library::Added added = library->addEntry(BookMeta::fromJson(parsed(details).toObject()), paper);
        if (!added.error.isEmpty()) {
            p->m_engine.throwError(added.error);
            return 0;
        }
        emit p->touchedLibrary();
        return double(added.id);
    }
    Q_INVOKABLE QString libraryNotes(double id, const QString &kind) {
        const Library *library = this->library(false);
        const Recap notes = library ? library->notes(qint64(id), kind) : Recap();
        return compact(QJsonObject{{"text", notes.text}, {"section", notes.section}, {"fraction", notes.fraction}, {"updated", notes.updated}});
    }
    Q_INVOKABLE void librarySetNotes(double id, const QString &kind, const QString &text) {
        Library *library = this->library(true);
        if (const auto book = this->book(library, id)) {
            const Progress place = library->progress(book->id);
            library->setNotes(book->id, kind, {place.section, place.fraction, text, 0});
        }
    }
    Q_INVOKABLE QString libraryProgress(double id) {
        const Library *library = this->library(false);
        const Progress place = library ? library->progress(qint64(id)) : Progress();
        return compact(QJsonObject{{"section", place.section}, {"fraction", place.fraction}, {"percent", place.percent}, {"updated", place.updated}});
    }
    Q_INVOKABLE QString librarySetProgress(double id, const QString &place) {
        Library *library = this->library(true);
        const auto book = own(library, id, "moved");
        if (!book)
            return "null";
        const QJsonObject given = parsed(place).toObject();
        Progress progress = library->progress(book->id);
        progress.section = qMax(0, given.value("section").toInt(progress.section));
        progress.fraction = qBound(0.0, given.value("fraction").toDouble(0), 1.0);
        progress.updated = 0;
        // How far into the book that is depends on how long the book is.
        for (const QString &format : book->formats) {
            if (!Formats::readable(format))
                continue;
            if (const auto document = Formats::open(library->formatPath(book->id, format))) {
                progress.section = qMin(progress.section, document->count() - 1);
                progress.percent = 100.0 * (progress.section + progress.fraction) / document->count();
            }
            break;
        }
        library->setProgress(book->id, progress);
        emit p->touchedLibrary();
        return libraryProgress(id);
    }
    Q_INVOKABLE QString libraryAnnotations(double id) {
        QJsonArray list;
        if (const Library *library = this->library(false))
            for (const Annotation &annotation : library->annotations(qint64(id)))
                list << QJsonObject{{"id", annotation.id}, {"kind", annotation.kind}, {"section", annotation.section},
                                    {"fraction", annotation.fraction}, {"text", annotation.text}, {"note", annotation.note},
                                    {"created", annotation.created}};
        return compact(list);
    }
    Q_INVOKABLE QString libraryToc(double id) {
        const auto document = open(id);
        if (!document)
            return "null";
        QJsonArray contents;
        for (const TocEntry &entry : document->toc())
            contents << QJsonObject{{"section", entry.section}, {"title", entry.title}, {"level", entry.level}};
        return compact(QJsonObject{{"sections", document->count()}, {"contents", contents}});
    }
    Q_INVOKABLE QString libraryText(double id, double section) {
        const auto document = open(id);
        if (!document)
            return {};
        if (section >= document->count()) {
            p->m_engine.throwError(QString("This book has sections 0 to %1.").arg(document->count() - 1));
            return {};
        }
        if (section >= 0)
            return document->text(int(section));
        QString text;
        for (int i = 0; i < document->count(); ++i)
            text += document->text(i) + "\n\n";
        return text;
    }

    // The knowledge graph. Any plugin may read it and write to it, the
    // general graph and scopes alike, as any plugin may change the library.
    // What went wrong is thrown into the script, as GraphOps words it.
    QJsonObject graphAnswer(const QJsonObject &answer) {
        if (GraphOps::failed(answer))
            p->m_engine.throwError(answer.value("error").toString());
        return answer;
    }
    static std::optional<GraphPlace> graphPlace(qint64 bookId, const QJsonObject &at) {
        if (bookId <= 0 || !at.contains("section"))
            return std::nullopt;
        return GraphPlace{bookId, at.value("section").toInt(), at.contains("fraction") ? at.value("fraction").toDouble() : 1.0};
    }
    Q_INVOKABLE QString graphFind(const QString &words, const QString &kind, const QString &scope, bool everywhere, double limit) {
        const Library *library = this->library(false);
        return compact(library ? GraphOps::find(*library, words, kind, scope, everywhere, int(limit)).value("nodes").toArray()
                               : QJsonArray());
    }
    Q_INVOKABLE QString graphNode(double id) {
        const Library *library = this->library(false);
        const auto node = library ? Graph(*library).node(qint64(id)) : std::nullopt;
        return node ? compact(node->toJson()) : QString("null");
    }
    Q_INVOKABLE QString graphAround(double id, double depth) {
        const Library *library = this->library(false);
        return compact(library ? Graph(*library).around(qint64(id), int(depth)).toJson() : GraphView().toJson());
    }
    Q_INVOKABLE QString graphOfBook(double id, const QString &upTo) {
        const Library *library = this->library(false);
        return compact(library ? Graph(*library).ofBook(qint64(id), graphPlace(qint64(id), parsed(upTo).toObject())).toJson()
                               : GraphView().toJson());
    }
    Q_INVOKABLE QString graphBeyond(double id, const QString &upTo) {
        const Library *library = this->library(false);
        return compact(library ? Graph(*library).beyond(qint64(id), graphPlace(qint64(id), parsed(upTo).toObject())).toJson()
                               : GraphView().toJson());
    }
    // One request to the model the user chose: what a book connects to in
    // the other books read. Answers how many connections it added.
    Q_INVOKABLE void graphConnect(double id, const QJSValue &resolve, const QJSValue &reject) {
        Library *library = this->library(true);
        const qint64 bookId = qint64(id);
        const auto at = library ? GraphBuild::builtTo(*library, bookId) : std::nullopt;
        const GraphLinks::Offer offer = at ? GraphLinks::offer(*library, bookId, *at) : GraphLinks::Offer();
        // Nothing to compare, or the same comparison as before: nothing is sent.
        if (!library || !at || !GraphLinks::due(*library, bookId)) {
            resolve.call({0});
            return;
        }
        QString why = "No AI is set up.";
        std::unique_ptr<AiProvider> provider = p->m_services.ai ? p->m_services.ai(&why) : nullptr;
        if (!provider) {
            reject.call({why});
            return;
        }
        auto *asking = new AiCall(std::move(provider), p);
        p->busy(1);
        connect(asking, &AiCall::done, p, [this, asking, resolve, bookId, at, offer](const QString &answer) {
            int added = 0;
            if (Library *now = this->library(false)) {
                const GraphLinks::Applied applied = GraphLinks::apply(*now, bookId, *at, offer, answer);
                GraphLinks::setConnected(*now, bookId, *at, offer);
                added = applied.links + applied.topics;
            }
            resolve.call({added});
            asking->deleteLater();
            p->busy(-1);
        });
        connect(asking, &AiCall::failed, p, [this, asking, reject](const QString &problem) {
            reject.call({problem});
            asking->deleteLater();
            p->busy(-1);
        });
        asking->ask(GraphLinks::request(*library, bookId, offer));
    }
    Q_INVOKABLE QString graphWhole(const QString &scope, double limit) {
        const Library *library = this->library(false);
        return compact(library ? Graph(*library).whole(scope, qMax(1, int(limit))).toJson() : GraphView().toJson());
    }
    Q_INVOKABLE double graphUpsert(const QString &thing) {
        Library *library = this->library(true);
        if (!library)
            return 0;
        return double(graphAnswer(GraphOps::add(*library, parsed(thing).toObject())).value("node").toObject().value("id").toInteger());
    }
    Q_INVOKABLE double graphLink(double source, double target, const QString &relation, const QString &details) {
        Library *library = this->library(true);
        if (!library)
            return 0;
        const QJsonObject given = parsed(details).toObject();
        return double(graphAnswer(GraphOps::link(*library, qint64(source), qint64(target), relation, given.value("note").toString(),
                                                 graphPlace(given.value("book").toInteger(), given)))
                          .value("edge").toInteger());
    }
    Q_INVOKABLE bool graphUnlink(double edge) {
        Library *library = this->library(true);
        return library && Graph(*library).unlink(qint64(edge));
    }
    Q_INVOKABLE bool graphMerge(double keep, double other) {
        Library *library = this->library(true);
        return library && Graph(*library).merge(qint64(keep), qint64(other));
    }
    Q_INVOKABLE bool graphRemove(double id) {
        Library *library = this->library(true);
        return library && Graph(*library).remove(qint64(id));
    }
    Q_INVOKABLE QString graphScopes() {
        const Library *library = this->library(false);
        return compact(QJsonArray::fromStringList(library ? Graph(*library).scopes() : QStringList()));
    }
    Q_INVOKABLE bool graphCopyScope(const QString &from, const QString &to) {
        Library *library = this->library(true);
        return library && Graph(*library).copyScope(from, to);
    }
    Q_INVOKABLE bool graphRemoveScope(const QString &scope) {
        Library *library = this->library(true);
        return library && Graph(*library).removeScope(scope);
    }
    Q_INVOKABLE QString graphExported(const QString &format, const QString &what) {
        Library *library = this->library(true);
        if (!library)
            return {};
        const QJsonObject of = parsed(what).toObject();
        const qint64 bookId = of.value("book").toInteger();
        return graphAnswer(GraphOps::exported(*library, bookId, of.value("scope").toString(),
                                              graphPlace(bookId, of.value("upTo").toObject()), format))
            .value("content").toString();
    }

    Q_INVOKABLE QString reading() {
        const QJsonObject reading = p->m_services.reading ? p->m_services.reading() : QJsonObject();
        return reading.isEmpty() ? QString("null") : compact(reading);
    }
    Q_INVOKABLE void openBook(double id, double section, double fraction) {
        emit p->openBookRequested(qint64(id), int(section), qBound(0.0, fraction, 1.0));
    }
    Q_INVOKABLE void reloadBook() { emit p->reloadBookRequested(0); }

    Q_INVOKABLE void uiAsk(const QString &title, const QString &placeholder, const QString &text, const QJSValue &resolve) {
        // With nobody to ask (a command, a check), the answer is no answer.
        static const QMetaMethod asked = QMetaMethod::fromSignal(&ScriptPlugin::askRequested);
        if (!p->isSignalConnected(asked)) {
            resolve.call({QJSValue(QJSValue::NullValue)});
            return;
        }
        const int token = p->m_nextToken++;
        p->m_asks.insert(token, resolve);
        p->busy(1);
        emit p->askRequested(token, title, placeholder, text);
    }

    // Why a form can't be drawn, for the script to throw at once; empty when it can.
    Q_INVOKABLE QString formProblem(const QString &json) {
        QString error;
        ScriptForm::checked(parsed(json).toObject(), &error);
        return error;
    }
    Q_INVOKABLE void uiForm(const QString &json, const QJSValue &resolve) {
        QString error;
        const QJsonObject form = ScriptForm::checked(parsed(json).toObject(), &error);
        // As for a question: nobody to fill it in is no answer.
        static const QMetaMethod asked = QMetaMethod::fromSignal(&ScriptPlugin::formRequested);
        if (form.isEmpty() || !p->isSignalConnected(asked)) {
            resolve.call({QJSValue(QJSValue::NullValue)});
            return;
        }
        const int token = p->m_nextToken++;
        p->m_asks.insert(token, resolve);
        p->busy(1);
        emit p->formRequested(token, form);
    }

    Q_INVOKABLE QString settings() { return compact(p->kept()); }
    Q_INVOKABLE void keep(const QString &key, const QString &json) {
        QJsonObject kept = p->kept();
        kept.insert(key, parsed(json));
        QSaveFile file(p->m_manifest.dataFolder() + "/settings.json");
        file.setDirectWriteFallback(false);
        const QByteArray contents = QJsonDocument(kept).toJson(QJsonDocument::Indented);
        if (!QDir().mkpath(p->m_manifest.dataFolder()) || !file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size()
            || !file.commit())
            p->m_engine.throwError("The plugin's settings can't be written to " + file.fileName() + ".");
    }
    Q_INVOKABLE QString options() { return compact(p->m_services.settings ? p->m_services.settings->options() : QJsonObject()); }
    Q_INVOKABLE bool setOption(const QString &key, const QString &json) {
        return p->m_services.settings && p->m_services.settings->setOption(key, parsed(json));
    }

    Q_INVOKABLE bool aiAvailable() { return p->m_services.ai && p->m_services.ai(nullptr) != nullptr; }
    Q_INVOKABLE void aiAsk(const QString &prompt, const QString &system, const QString &model, const QJSValue &onText,
                           const QJSValue &resolve, const QJSValue &reject) {
        QString why = "No AI is set up.";
        std::unique_ptr<AiProvider> provider = p->m_services.ai ? p->m_services.ai(&why) : nullptr;
        if (!provider) {
            reject.call({why});
            return;
        }
        auto *asking = new AiCall(std::move(provider), p);
        p->busy(1);
        auto finish = [this, asking](const QJSValue &callback, const QString &text) {
            callback.call({text});
            asking->deleteLater();
            p->busy(-1);
        };
        connect(asking, &AiCall::done, p, [finish, resolve](const QString &answer) { finish(resolve, answer); });
        connect(asking, &AiCall::failed, p, [finish, reject](const QString &problem) { finish(reject, problem); });
        // The answer as it is written, for a script that shows it growing.
        if (onText.isCallable())
            connect(asking, &AiCall::delta, p, [this, onText](const QString &piece) {
                QString problem;
                p->call(onText, {piece}, &problem);
                if (!problem.isEmpty())
                    p->report(problem);
            });
        AiRequest request;
        request.system = system;
        request.messages.append({"user", prompt});
        // What a plugin writes is for a person to read, so the main model
        // unless it says otherwise: "quick", or a model by name.
        if (!model.isEmpty())
            Ai::useModel(request, model);
        asking->ask(request);
    }

    Q_INVOKABLE void http(const QString &method, const QString &address, const QString &body, const QString &headers,
                          const QJSValue &resolve, const QJSValue &reject) {
        const QUrl url(address);
        if (!allowed("http") || !p->m_services.network || !url.isValid() || !QStringList{"http", "https"}.contains(url.scheme())) {
            reject.call({"\"" + address + "\" can't be fetched: an address starts with http:// or https://."});
            return;
        }
        QNetworkRequest request(url);
        request.setTransferTimeout(30000);
        // Nothing about the reader goes along: every plugin is the same client.
        request.setHeader(QNetworkRequest::UserAgentHeader, "omabook-plugin/" OMABOOK_VERSION);
        const QJsonObject given = parsed(headers).toObject();
        for (auto it = given.begin(); it != given.end(); ++it)
            request.setRawHeader(it.key().toUtf8(), it.value().toVariant().toString().toUtf8());
        QNetworkReply *reply = method == "POST" ? p->m_services.network->post(request, body.toUtf8())
                                                : p->m_services.network->get(request);
        p->busy(1);
        connect(reply, &QNetworkReply::finished, p, [this, reply, resolve, reject] {
            reply->deleteLater();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status == 0) {
                // No answer at all is a failure; an answer of 404 is an answer.
                reject.call({reply->url().host() + " can't be reached: " + reply->errorString()});
            } else {
                QJsonObject headers;
                for (const auto &header : reply->rawHeaderPairs())
                    headers.insert(QString::fromUtf8(header.first).toLower(), QString::fromUtf8(header.second));
                resolve.call({compact(QJsonObject{{"status", status}, {"ok", status >= 200 && status < 300},
                                                  {"text", QString::fromUtf8(reply->readAll())}, {"headers", headers}})});
            }
            p->busy(-1);
        });
    }

    Q_INVOKABLE QJSValue fileRead(const QString &name) {
        if (!allowed("files"))
            return QJSValue(QJSValue::NullValue);
        const QString path = p->inside(name, false);
        QFile file(path);
        if (path.isEmpty()) {
            p->m_engine.throwError("\"" + name + "\" is outside the plugin's folder.");
            return {};
        }
        return file.open(QIODevice::ReadOnly) ? QJSValue(QString::fromUtf8(file.readAll())) : QJSValue(QJSValue::NullValue);
    }
    Q_INVOKABLE bool fileWrite(const QString &name, const QString &text) {
        const QString path = allowed("files") ? p->inside(name, true) : QString();
        if (path.isEmpty()) {
            p->m_engine.throwError("\"" + name + "\" can't be written: a plugin writes under its data/ folder, as in \"data/notes.txt\".");
            return false;
        }
        QSaveFile file(path);
        file.setDirectWriteFallback(false);
        const QByteArray contents = text.toUtf8();
        return QDir().mkpath(QFileInfo(path).absolutePath()) && file.open(QIODevice::WriteOnly)
               && file.write(contents) == contents.size() && file.commit();
    }
    Q_INVOKABLE QString fileList(const QString &name) {
        const QString path = allowed("files") ? p->inside(name, false) : QString();
        return compact(path.isEmpty() ? QJsonArray()
                                      : QJsonArray::fromStringList(QDir(path).entryList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name)));
    }
    Q_INVOKABLE bool fileExists(const QString &name) {
        const QString path = allowed("files") ? p->inside(name, false) : QString();
        return !path.isEmpty() && QFileInfo::exists(path);
    }
    Q_INVOKABLE bool fileRemove(const QString &name) {
        const QString path = allowed("files") ? p->inside(name, true) : QString();
        return !path.isEmpty() && QFileInfo(path).isFile() && QFile::remove(path);
    }

    Q_INVOKABLE void run(const QString &program, const QString &arguments, const QString &options, const QJSValue &resolve,
                         const QJSValue &reject) {
        if (!allowed("run")) {
            reject.call({QString("This plugin may not start programs.")});
            return;
        }
        const QJsonObject given = parsed(options).toObject();
        auto *process = new QProcess(p);
        auto *timeout = new QTimer(process);
        timeout->setSingleShot(true);
        process->setWorkingDirectory(p->m_manifest.folder);
        p->busy(1);
        // Whichever comes first settles it, once.
        auto finish = [this, process](const QJSValue &callback, const QString &answer) {
            if (process->property("settled").toBool())
                return;
            process->setProperty("settled", true);
            callback.call({answer});
            process->deleteLater();
            p->busy(-1);
        };
        connect(process, &QProcess::finished, p, [process, finish, resolve](int code) {
            finish(resolve, compact(QJsonObject{{"code", code},
                                                {"stdout", QString::fromUtf8(process->readAllStandardOutput())},
                                                {"stderr", QString::fromUtf8(process->readAllStandardError())}}));
        });
        connect(process, &QProcess::errorOccurred, p, [process, finish, reject, program](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
                finish(reject, program + " can't be started: " + process->errorString());
        });
        connect(timeout, &QTimer::timeout, p, [process, finish, reject, program] {
            process->kill();
            finish(reject, program + " took too long and was stopped.");
        });
        process->start(program, stringList(parsed(arguments)));
        if (given.contains("input"))
            process->write(given.value("input").toString().toUtf8());
        process->closeWriteChannel();
        timeout->start(std::clamp(given.value("timeout").toInt(30), 1, 600) * 1000);
    }

private:
    bool allowed(const QString &permission) const { return p->m_manifest.permissions.contains(permission); }

    // A file of the plugin's data folder, as create and replaceFile take
    // one; empty, with the script told why, for anything else.
    QString dataFile(const QString &name) {
        const QString file = allowed("files") ? p->inside(name, true) : QString();
        if (file.isEmpty() || !QFileInfo(file).isFile()) {
            p->m_engine.throwError("\"" + name + "\" is not a file under the plugin's data/ folder, which is where a book it makes comes from.");
            return {};
        }
        return file;
    }
    // A book this plugin made, which is the only kind it may rewrite or
    // remove: a book a person added, or another plugin made, is not its to touch.
    std::optional<Book> own(Library *library, double id, const QString &done) {
        const auto book = this->book(library, id);
        if (!book)
            return std::nullopt;
        const QString maker = book->identifiers.value("generated").toString();
        if (maker == p->m_manifest.id)
            return book;
        p->m_engine.throwError(QString("Book %1 (%2) was not made by this plugin%3, so it can't be %4 by it.")
                                   .arg(book->id).arg(book->meta.title, maker.isEmpty() ? QString() : " but by " + maker, done));
        return std::nullopt;
    }

    // The book a handle of the script's stands for, while the call it was
    // made for is still running.
    const BookSource *book(int token) {
        const BookSource *book = p->m_books.value(token);
        if (!book)
            p->m_engine.throwError(QString("That book is no longer open: a format's source is only good while its open or claims runs."));
        return book;
    }

    // The open library. Reading from none answers nothing; changing none is
    // an error the script hears of.
    Library *library(bool changing) {
        Library *library = p->m_services.library ? p->m_services.library() : nullptr;
        if (!library && changing)
            p->m_engine.throwError(QString("There is no library open."));
        return library;
    }
    std::optional<Book> book(Library *library, double id) {
        if (!library)
            return std::nullopt;
        const auto book = library->book(qint64(id));
        if (!book)
            p->m_engine.throwError(QString("There is no book %1 in the library.").arg(qint64(id)));
        return book;
    }
    std::unique_ptr<Document> open(double id) {
        const Library *library = this->library(true);
        const auto book = library ? library->book(qint64(id)) : std::nullopt;
        if (!book) {
            if (library)
                p->m_engine.throwError(QString("There is no book %1 in the library.").arg(qint64(id)));
            return nullptr;
        }
        QString error = QString("Book %1 has no format Omabook reads itself.").arg(book->id);
        std::unique_ptr<Document> document;
        for (const QString &format : book->formats) {
            if (Formats::readable(format)) {
                document = Formats::open(library->formatPath(book->id, format), &error);
                break;
            }
        }
        if (!document)
            p->m_engine.throwError(error);
        return document;
    }

    ScriptPlugin *p;
};

ScriptGate::ScriptGate(ScriptPlugin *plugin) : m_plugin(plugin), m_thread(plugin->thread()) {}

void ScriptGate::close() {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_plugin = nullptr;
    }
    m_wake.notify_all();
}

bool ScriptGate::run(const std::function<void(ScriptPlugin &)> &work, std::chrono::milliseconds patience, QString *error) {
    const QString gone = "The plugin that reads this format was switched off or changed while the book was being opened.";
    if (QThread::currentThread() == m_thread) {
        // Already where the script lives: nothing to wait for. (Closing
        // happens on this thread too, so the plugin can't go meanwhile.)
        ScriptPlugin *plugin = nullptr;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            plugin = m_plugin;
        }
        if (!plugin) {
            *error = gone;
            return false;
        }
        work(*plugin);
        return true;
    }
    struct Asked {
        bool done = false;
        bool givenUp = false;
    };
    const auto asked = std::make_shared<Asked>();
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_plugin) {
        *error = gone;
        return false;
    }
    // Posted under the lock, so the plugin is alive to be posted to; if it
    // goes before the event is delivered, the event goes with it and the
    // closing wakes us.
    QMetaObject::invokeMethod(m_plugin, [self = shared_from_this(), asked, work] {
        ScriptPlugin *plugin = nullptr;
        {
            const std::lock_guard<std::mutex> lock(self->m_mutex);
            if (!asked->givenUp)
                plugin = self->m_plugin;
        }
        if (!plugin)
            return;
        work(*plugin);
        {
            const std::lock_guard<std::mutex> lock(self->m_mutex);
            asked->done = true;
        }
        self->m_wake.notify_all();
    }, Qt::QueuedConnection);
    m_wake.wait_for(lock, patience, [&] { return asked->done || !m_plugin; });
    if (asked->done)
        return true;
    // Not for ever: the plugin's thread may be busy for long, or waiting
    // for this very thread to finish, and then nobody would ever move.
    asked->givenUp = true;
    *error = m_plugin ? "The plugin that reads this format didn't answer in time; its thread was busy." : gone;
    return false;
}

ScriptPlugin::ScriptPlugin(const ScriptManifest &manifest, const ScriptServices &services, QObject *parent)
    : QObject(parent), m_manifest(manifest), m_services(services), m_host(std::make_unique<ScriptHost>(this)),
      m_gate(std::make_shared<ScriptGate>(this)),
      m_watchdog(&m_engine, [this] {
          // On the watchdog's thread: the plugin hears of it on its own.
          QMetaObject::invokeMethod(this, [this] { interrupted(); }, Qt::QueuedConnection);
      }) {
    QJSEngine::setObjectOwnership(m_host.get(), QJSEngine::CppOwnership);
}

ScriptPlugin::~ScriptPlugin() {
    // Whoever waits on another thread for one of the script's formats is
    // let go before anything of the plugin is.
    m_gate->close();
    // Timers, requests and programs the script started hold functions of
    // the engine, so they go before it does.
    qDeleteAll(findChildren<QObject *>(QString(), Qt::FindDirectChildrenOnly));
}

bool ScriptPlugin::load(QString *problem) {
    if (!m_manifest.problem.isEmpty()) {
        *problem = m_manifest.problem;
        return false;
    }
    QFile bundled(":/scriptprelude.js"), script(m_manifest.folder + "/" + m_manifest.script);
    if (!bundled.open(QIODevice::ReadOnly) || !script.open(QIODevice::ReadOnly)) {
        *problem = "The script " + m_manifest.script + " can't be read.";
        return false;
    }
    m_parse = m_engine.evaluate("JSON.parse");
    const QJSValue prelude = m_engine.evaluate(QString::fromUtf8(bundled.readAll()), preludeName);
    const QJsonObject info{
        {"plugin", QJsonObject{{"id", m_manifest.id}, {"name", m_manifest.name}, {"version", m_manifest.version},
                               {"folder", m_manifest.folder}, {"dataFolder", m_manifest.dataFolder()},
                               {"permissions", QJsonArray::fromStringList(m_manifest.permissions)}}},
        {"permissions", QJsonArray::fromStringList(m_manifest.permissions)},
        {"events", QJsonArray::fromStringList(ScriptApi::eventNames())},
        {"commandPlaces", QJsonArray::fromStringList(ScriptApi::commandPlaces())},
        {"panelPlaces", QJsonArray::fromStringList(ScriptApi::panelPlaces())},
    };
    const QJSValue built = prelude.call({m_engine.newQObject(m_host.get()), fromJson(info)});
    if (built.isError()) {
        *problem = "Omabook's own side of the plugin didn't start: " + built.toString();
        return false;
    }
    m_settle = built.property("settle");
    m_sourceHandle = built.property("source");
    m_bookJson = built.property("book");
    m_engine.globalObject().setProperty("omabook", built.property("omabook"));
    m_engine.globalObject().setProperty("console", built.property("console"));
    m_engine.globalObject().setProperty("globalThis", m_engine.globalObject());

    enter();
    const QJSValue result = m_engine.evaluate(QString::fromUtf8(script.readAll()), m_manifest.script);
    if (m_engine.isInterrupted()) {
        m_engine.setInterrupted(false);
        m_depth = 0;
        *problem = QString("%1 ran for more than %2 seconds without finishing and was stopped.")
                       .arg(m_manifest.script).arg(m_limit.count() / 1000.0);
        return false;
    }
    leave();
    if (result.isError()) {
        *problem = describe(result);
        return false;
    }
    return true;
}

QStringList ScriptPlugin::events() const {
    QStringList events;
    for (const auto &[event, handlers] : m_handlers.asKeyValueRange())
        if (!handlers.isEmpty())
            events << event;
    events.sort();
    return events;
}

QJSValue ScriptPlugin::fromJson(const QJsonValue &value) { return m_parse.call({compact(value)}); }

// "main.js:12: TypeError: ...": where the script went wrong, which is the
// first place in the error's trail that is the script's own.
QString ScriptPlugin::describe(const QJSValue &error) const {
    if (!error.isError())
        return error.toString();
    QString where;
    for (const QString &frame : error.property("stack").toString().split('\n')) {
        QString place = frame.section('@', -1);
        if (place.startsWith("file:"))
            place.remove(0, 5);
        if (place.isEmpty() || place.startsWith(preludeName) || place.startsWith(':'))
            continue;
        where = place;
        break;
    }
    return where.isEmpty() ? error.toString() : where + ": " + error.toString();
}

// Each stretch of a plugin's own code is one write to the library: a
// plugin that notes forty things in the knowledge graph makes one change,
// not forty, which is much faster and wakes the window once.
void ScriptPlugin::enter() {
    if (m_depth++ == 0) {
        m_watchdog.arm(m_limit);
        if (Library *library = m_services.library ? m_services.library() : nullptr)
            m_batch.emplace(*library);
    }
}

void ScriptPlugin::leave() {
    if (m_depth > 0 && --m_depth == 0) {
        m_watchdog.disarm();
        m_batch.reset();
    }
}

// The watchdog stopped the script while nothing of Omabook's was waiting
// for it: a function it gave a promise or a timer.
void ScriptPlugin::interrupted() {
    if (!m_engine.isInterrupted())
        return;
    m_engine.setInterrupted(false);
    m_depth = 0;
    report(QString("The script ran for more than %1 seconds without finishing and was stopped.").arg(m_limit.count() / 1000.0));
}

QJSValue ScriptPlugin::call(const QJSValue &function, const QJSValueList &arguments, QString *problem) {
    if (!function.isCallable()) {
        *problem = "That is not a function.";
        return {};
    }
    const int depth = m_depth;
    // Left over from a script stopped just as it finished.
    if (depth == 0 && m_engine.isInterrupted())
        m_engine.setInterrupted(false);
    enter();
    const QJSValue result = function.call(arguments);
    if (m_engine.isInterrupted()) {
        // Whatever the script was in the middle of never got to leave.
        m_engine.setInterrupted(false);
        m_depth = depth;
        *problem = QString("The script ran for more than %1 seconds without finishing and was stopped.").arg(m_limit.count() / 1000.0);
        return {};
    }
    leave();
    if (result.isError()) {
        *problem = describe(result);
        return {};
    }
    return result;
}

void ScriptPlugin::invoke(const QJSValue &function, const QJsonValue &argument, const Done &done) {
    auto fail = [&](const QString &problem) {
        if (done)
            done({}, problem);
        else
            report(problem);
    };
    QString problem;
    const QJSValue result = call(function, {fromJson(argument)}, &problem);
    if (!problem.isEmpty()) {
        fail(problem);
        return;
    }
    // A value or a promise of one: either way the answer comes back through
    // the event loop, so a caller is never answered before it has asked.
    const int token = m_nextToken++;
    m_waiting.insert(token, [this, done](const QJsonValue &answer, const QString &why) {
        if (done)
            done(answer, why);
        else if (!why.isEmpty())
            report(why);
    });
    busy(1);
    call(m_settle, {token, result}, &problem);
    if (!problem.isEmpty()) {
        m_waiting.remove(token);
        busy(-1);
        fail(problem);
    }
}

void ScriptPlugin::emitEvent(const QString &event, const QJsonObject &payload) {
    // A handler may take itself off the list while the list is walked.
    const QList<QJSValue> handlers = m_handlers.value(event);
    for (const QJSValue &handler : handlers)
        invoke(handler, payload, {});
}

void ScriptPlugin::search(const QString &source, const QJsonObject &book, const Done &done) {
    for (const Source &known : std::as_const(m_sources)) {
        if (known.name == source) {
            invoke(known.search, book, done);
            return;
        }
    }
    done({}, m_manifest.name + " has no source called " + source + ".");
}

void ScriptPlugin::makeCatalog(const QString &format, const QJsonArray &books, const Done &done) {
    for (const Catalog &known : std::as_const(m_catalogs)) {
        if (known.format == format) {
            invoke(known.make, books, done);
            return;
        }
    }
    done({}, m_manifest.name + " has no catalog format " + format + ".");
}

void ScriptPlugin::imported(const QString &importer, const QJsonObject &book, const Done &done) {
    for (const Importer &known : std::as_const(m_importers)) {
        if (known.name == importer) {
            invoke(known.added, book, done);
            return;
        }
    }
    done({}, m_manifest.name + " has no importer called " + importer + ".");
}

void ScriptPlugin::runCommand(const QString &id, const QJsonObject &context, const Done &done) {
    for (const Command &known : std::as_const(m_commands)) {
        // By its whole id, or by the part the script gave it.
        if (known.id == id || known.id.section(':', -1) == id) {
            invoke(known.run, context, done);
            return;
        }
    }
    if (done)
        done({}, m_manifest.name + " has no command " + id + ".");
}

void ScriptPlugin::callFunction(const QString &name, const QJsonArray &arguments, const Done &done) {
    QString problem;
    QJSValueList list;
    for (const QJsonValue &argument : arguments)
        list << fromJson(argument);
    const QJSValue function = m_engine.globalObject().property(name);
    if (!function.isCallable()) {
        done({}, m_manifest.script + " has no function " + name + " at its top level.");
        return;
    }
    const QJSValue result = call(function, list, &problem);
    if (!problem.isEmpty()) {
        done({}, problem);
        return;
    }
    const int token = m_nextToken++;
    m_waiting.insert(token, done);
    busy(1);
    call(m_settle, {token, result}, &problem);
}

QJsonValue ScriptPlugin::callNow(const QString &name, const QJsonArray &arguments, QString *problem) {
    QJSValueList list;
    for (const QJsonValue &argument : arguments)
        list << fromJson(argument);
    const QJSValue function = m_engine.globalObject().property(name);
    if (!function.isCallable()) {
        *problem = m_manifest.script + " has no function " + name + " at its top level.";
        return {};
    }
    const QJSValue result = call(function, list, problem);
    if (!problem->isEmpty() || result.isUndefined())
        return {};
    QString unused;
    const QJSValue json = call(m_engine.evaluate("JSON.stringify"), {result}, &unused);
    return json.isString() ? parsed(json.toString()) : QJsonValue();
}

bool ScriptPlugin::claimsBook(const QString &format, const BookSource &source) {
    for (const Format &known : std::as_const(m_formats)) {
        if (known.name != format || !known.claims.isCallable())
            continue;
        const int token = m_nextToken++;
        m_books.insert(token, &source);
        QString problem;
        const QJSValue handle = call(m_sourceHandle, {token, source.name(), source.isFolder()}, &problem);
        const QJSValue claimed = problem.isEmpty() ? call(known.claims, {handle}, &problem) : QJSValue();
        m_books.remove(token);
        // A claims that throws claims nothing; what it threw is the plugin's to hear.
        if (!problem.isEmpty())
            report(problem);
        return problem.isEmpty() && claimed.toBool();
    }
    return false;
}

QJsonObject ScriptPlugin::openBook(const QString &format, const BookSource &source, QString *problem) {
    for (const Format &known : std::as_const(m_formats)) {
        if (known.name != format)
            continue;
        const int token = m_nextToken++;
        m_books.insert(token, &source);
        const QJSValue handle = call(m_sourceHandle, {token, source.name(), source.isFolder()}, problem);
        const QJSValue book = problem->isEmpty() ? call(known.open, {handle}, problem) : QJSValue();
        const QJSValue json = problem->isEmpty() ? call(m_bookJson, {book}, problem) : QJSValue();
        // The handle is good for nothing from here on, kept or not.
        m_books.remove(token);
        return problem->isEmpty() ? parsed(json.toString()).toObject() : QJsonObject();
    }
    *problem = m_manifest.name + " has no format called " + format + ".";
    return {};
}

std::optional<BookDescription> ScriptPlugin::described(const QJsonObject &book, QString *problem) {
    BookDescription description;
    QJsonObject details = book;
    if (details.value("published").isDouble())
        details.insert("published", QString::number(details.value("published").toInt()));
    if (details.value("authors").isString())
        details.insert("authors", QJsonArray::fromStringList(BookMeta::splitAuthors(details.value("authors").toString())));
    description.meta = BookMeta::fromJson(details);
    description.cover = book.value("cover").toString();
    for (const QJsonValue &value : book.value("chapters").toArray()) {
        const QJsonObject chapter = value.toObject();
        // Whichever the script found easiest to give: HTML as it is,
        // Markdown and plain text made into HTML here.
        const QString html = chapter.value("html").isString()       ? chapter.value("html").toString()
                             : chapter.value("markdown").isString() ? htmlFromMarkdown(chapter.value("markdown").toString())
                                                                    : htmlFromText(chapter.value("text").toString());
        description.chapters.append({chapter.value("title").toString().simplified(), html, chapter.value("file").toString(),
                                     std::clamp(chapter.value("level").toInt(), 0, 8)});
    }
    if (description.chapters.isEmpty()) {
        *problem = "open answered a book with no chapters in it.";
        return std::nullopt;
    }
    return description;
}

FormatProvider ScriptPlugin::provider(const Format &format) const {
    FormatProvider provider;
    provider.id = "plugin:" + m_manifest.id + "/" + format.name;
    provider.name = format.name;
    provider.extensions = format.extensions;
    // The script may take its whole time, and the thread it runs on as
    // long again to get to it.
    const std::chrono::milliseconds patience = m_limit + std::max(m_limit, std::chrono::milliseconds(1000));
    const QString name = format.name, plugin = m_manifest.id;
    // Nothing of the plugin is held here but its gate: the book goes in by
    // value and what comes back is plain data, so a caller that gave up
    // leaves nothing dangling.
    if (format.claims.isCallable()) {
        provider.claims = [gate = m_gate, name, patience](const BookSource &source) {
            const auto claimed = std::make_shared<bool>(false);
            QString error;
            const bool asked = gate->run([claimed, name, source](ScriptPlugin &plugin) { *claimed = plugin.claimsBook(name, source); },
                                         patience, &error);
            return asked && *claimed;
        };
    }
    provider.describe = [gate = m_gate, name, plugin, patience](const BookSource &source, QString *error) -> std::optional<BookDescription> {
        struct Answer {
            QJsonObject book;
            QString problem;
        };
        const auto answer = std::make_shared<Answer>();
        QString why;
        if (gate->run([answer, name, source](ScriptPlugin &plugin) { answer->book = plugin.openBook(name, source, &answer->problem); },
                      patience, &why))
            why = answer->problem;
        std::optional<BookDescription> description;
        if (why.isEmpty())
            description = described(answer->book, &why);
        if (!description && error)
            *error = QString("%1 (the plugin %2) couldn't open %3: %4").arg(name, plugin, QFileInfo(source.path()).fileName(), why);
        return description;
    };
    return provider;
}

void ScriptPlugin::answerAsk(int token, const QJsonValue &text) {
    if (!m_asks.contains(token))
        return;
    const QJSValue resolve = m_asks.take(token);
    resolve.call({text.isString() ? QJSValue(text.toString()) : QJSValue(QJSValue::NullValue)});
    busy(-1);
}

void ScriptPlugin::answerForm(int token, const QJsonValue &values) {
    if (!m_asks.contains(token))
        return;
    const QJSValue resolve = m_asks.take(token);
    resolve.call({values.isObject() ? QJSValue(compact(values)) : QJSValue(QJSValue::NullValue)});
    busy(-1);
}

void ScriptPlugin::launch(const QString &id, const Done &done) {
    for (const Launcher &known : std::as_const(m_launchers)) {
        if (known.id == id) {
            invoke(known.open, QJsonObject{{"launcher", id}}, done);
            return;
        }
    }
    if (done)
        done({}, m_manifest.name + " has no launcher " + id + ".");
}

void ScriptPlugin::dropLauncher(const QString &id) {
    const auto found = std::ranges::find(m_launchers, id, &Launcher::id);
    if (found == m_launchers.end())
        return;
    const QJSValue removed = found->removed;
    m_launchers.erase(found);
    emit registered();
    if (removed.isCallable())
        invoke(removed, QJsonObject{{"launcher", id}}, {});
}

QStringList ScriptPlugin::takeLog() {
    const QStringList log = m_log;
    m_log.clear();
    return log;
}

QStringList ScriptPlugin::surface() {
    const QJSValue paths = m_engine.evaluate(
        "(function () { var out = []; (function walk(object, prefix) { Object.keys(object).forEach(function (key) {"
        " var value = object[key]; if (value !== null && typeof value === 'object' && prefix + key !== 'plugin')"
        " walk(value, prefix + key + '.'); else out.push(prefix + key); }); })(omabook, ''); return out.join(','); })()");
    return paths.toString().split(',', Qt::SkipEmptyParts);
}

void ScriptPlugin::busy(int change) {
    m_busy = qMax(0, m_busy + change);
    if (m_busy == 0)
        emit settled();
}

void ScriptPlugin::report(const QString &problem) {
    m_log << "error: " + problem;
    emit failed(problem);
}

QString ScriptPlugin::inside(const QString &path, bool writing) const {
    const QString root = QFileInfo(m_manifest.folder).canonicalFilePath();
    if (root.isEmpty())
        return {};
    const QString target = QDir::cleanPath(QDir(root).absoluteFilePath(path));
    // A link is followed before it is judged, so one pointing out of the
    // folder leads nowhere. What doesn't exist yet is judged by the nearest
    // folder that does.
    QString existing = target, rest;
    while (!QFileInfo::exists(existing) && existing != "/") {
        rest = "/" + QFileInfo(existing).fileName() + rest;
        existing = QFileInfo(existing).absolutePath();
    }
    const QString real = QFileInfo(existing).canonicalFilePath() + rest;
    const QString allowed = writing ? root + "/data" : root;
    if (real == allowed)
        return writing ? QString() : real;
    return real.startsWith(allowed + "/") ? real : QString();
}

QJsonObject ScriptPlugin::kept() const {
    QFile file(m_manifest.dataFolder() + "/settings.json");
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

#include "scriptplugin.moc"
