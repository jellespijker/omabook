#include "scriptfiles.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QPointer>
#include <QSaveFile>
#include <algorithm>
#include <optional>
#include <tuple>
#include <utility>

#include "library.h"
#include "pluginfiles.h"
#include "scriptdesk.h"
#include "scriptplugin.h"

using namespace Qt::StringLiterals;

namespace {

// The most a plugin holds at once of what the person picked or copied: the
// oldest goes first.
constexpr std::size_t heldAtMost = 32;

[[nodiscard]] QString compact(const QJsonValue &value) {
    return QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact)).mid(1).chopped(1);
}

[[nodiscard]] QString megabytes(qint64 bytes) { return QString::number(bytes / (qint64{1024} * 1024)) + u" MB"_s; }

// How large a picture is, when what is held is one; invalid when it isn't.
template <typename Held> [[nodiscard]] QSize pictureSize(const Held &held) {
    if (!held.path.isEmpty())
        return QImageReader(held.path).size();
    QByteArray bytes = held.bytes;
    QBuffer buffer(&bytes);
    return QImageReader(&buffer).size();
}

} // namespace

QJSValue ScriptFiles::build(QJSEngine &engine) {
    QFile wrapper(u":/scriptfiles.js"_s);
    if (!wrapper.open(QIODevice::ReadOnly))
        return engine.newErrorObject(QJSValue::GenericError, u"Omabook's own side of omabook.files is missing."_s);
    QJSEngine::setObjectOwnership(this, QJSEngine::CppOwnership);
    const QJSValue made = engine.evaluate(QString::fromUtf8(wrapper.readAll()), u"omabook-api/files"_s);
    return made.call({engine.newQObject(this), engine.toScriptValue(m_plugin->m_manifest.permissions)});
}

void ScriptFiles::fail(const QString &why) { m_plugin->m_engine.throwError(why); }

bool ScriptFiles::allowed(const QString &permission) const { return m_plugin->m_manifest.permissions.contains(permission); }

bool ScriptFiles::acting(const QString &what) {
    if (m_plugin->acting())
        return true;
    fail(u"%1 only right after the person did something of the plugin's: ran one of its commands, or used its view."_s.arg(what));
    return false;
}

QString ScriptFiles::writable(const QString &path) {
    const QString real = allowed(u"files"_s) ? m_plugin->inside(path, true) : QString();
    if (real.isEmpty())
        fail(u"\"%1\" can't be written: a plugin writes under its data/ folder, as in \"data/notes.txt\"."_s.arg(path));
    return real;
}

bool ScriptFiles::put(const QString &path, const QByteArray &bytes) {
    const QString real = writable(path);
    if (real.isEmpty())
        return false;
    if (bytes.size() > PluginFiles::maxFileBytes) {
        fail(u"\"%1\" can't be written: a file of a plugin's holds at most %2."_s.arg(path, megabytes(PluginFiles::maxFileBytes)));
        return false;
    }
    const QString data = m_plugin->m_manifest.dataFolder();
    const qint64 before = QFileInfo(real).isFile() ? QFileInfo(real).size() : 0;
    if (PluginFiles::used(data) - before + bytes.size() > PluginFiles::maxPluginBytes) {
        fail(u"\"%1\" can't be written: a plugin's data/ folder holds at most %2."_s.arg(path, megabytes(PluginFiles::maxPluginBytes)));
        return false;
    }
    QSaveFile file(real);
    file.setDirectWriteFallback(false);
    return QDir().mkpath(QFileInfo(real).absolutePath()) && file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()
           && file.commit();
}

std::optional<QByteArray> ScriptFiles::bytesOf(const QString &path) {
    if (!allowed(u"files"_s))
        return std::nullopt;
    const QString real = m_plugin->inside(path, false);
    if (real.isEmpty()) {
        fail(u"\"%1\" is outside the plugin's folder."_s.arg(path));
        return std::nullopt;
    }
    QFile file(real);
    if (file.size() > PluginFiles::maxFileBytes) {
        fail(u"\"%1\" is larger than the %2 a plugin reads at once."_s.arg(path, megabytes(PluginFiles::maxFileBytes)));
        return std::nullopt;
    }
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    return file.readAll();
}

QJSValue ScriptFiles::read(const QString &path) {
    const auto bytes = bytesOf(path);
    return bytes ? QJSValue(QString::fromUtf8(*bytes)) : QJSValue(QJSValue::NullValue);
}

QJSValue ScriptFiles::readBytes(const QString &path) {
    const auto bytes = bytesOf(path);
    return bytes ? m_plugin->m_engine.toScriptValue(*bytes) : QJSValue(QJSValue::NullValue);
}

QJSValue ScriptFiles::readBase64(const QString &path) {
    const auto bytes = bytesOf(path);
    return bytes ? QJSValue(QString::fromLatin1(bytes->toBase64())) : QJSValue(QJSValue::NullValue);
}

bool ScriptFiles::write(const QString &path, const QString &text) { return put(path, text.toUtf8()); }

bool ScriptFiles::writeBytes(const QString &path, const QByteArray &bytes) { return put(path, bytes); }

bool ScriptFiles::writeBase64(const QString &path, const QString &base64) {
    const auto decoded = QByteArray::fromBase64Encoding(base64.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded) {
        fail(u"What is to be written to \"%1\" is no base64."_s.arg(path));
        return false;
    }
    return put(path, *decoded);
}

QString ScriptFiles::list(const QString &folder) {
    const QString real = allowed(u"files"_s) ? m_plugin->inside(folder, false) : QString();
    return compact(real.isEmpty() ? QJsonArray()
                                  : QJsonArray::fromStringList(QDir(real).entryList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name)));
}

bool ScriptFiles::exists(const QString &path) {
    const QString real = allowed(u"files"_s) ? m_plugin->inside(path, false) : QString();
    return !real.isEmpty() && QFileInfo::exists(real);
}

bool ScriptFiles::remove(const QString &path) {
    const QString real = allowed(u"files"_s) ? m_plugin->inside(path, true) : QString();
    if (real.isEmpty() || !QFileInfo(real).isFile())
        return false;
    const QString hash = Library::hashFile(real);
    if (!QFile::remove(real))
        return false;
    // So it goes on one's other computers too.
    const QString under = QDir(QFileInfo(m_plugin->m_manifest.dataFolder()).canonicalFilePath()).relativeFilePath(real);
    if (const Library *library = m_plugin->m_services.library ? m_plugin->m_services.library() : nullptr)
        std::ignore = PluginFiles::forget(*library, m_plugin->m_manifest.id, under, hash); // a removal not recorded comes back at a sync
    return true;
}

QString ScriptFiles::usage() {
    return compact(QJsonObject{{"used", allowed(u"files"_s) ? PluginFiles::used(m_plugin->m_manifest.dataFolder()) : 0},
                               {"file", PluginFiles::maxFileBytes},
                               {"plugin", PluginFiles::maxPluginBytes}});
}

QJsonObject ScriptFiles::hold(Held held, const QString &name) {
    held.id = m_next++;
    const QMimeDatabase types;
    const bool file = !held.path.isEmpty();
    QJsonObject described{{"id", held.id},
                          {"name", name},
                          {"size", file ? QFileInfo(held.path).size() : held.bytes.size()},
                          {"type", file ? types.mimeTypeForFile(held.path).name() : types.mimeTypeForData(held.bytes).name()}};
    if (const QSize size = pictureSize(held); size.isValid()) {
        described.insert("width", size.width());
        described.insert("height", size.height());
    }
    m_held.push_back(std::move(held));
    if (m_held.size() > heldAtMost)
        m_held.pop_front();
    return described;
}

void ScriptFiles::pick(const QString &kinds, bool multiple, const QJSValue &answered) {
    if (!allowed(u"files"_s)) {
        fail(u"A plugin asks for a file only with the files permission."_s);
        return;
    }
    ScriptDesk *desk = m_plugin->m_services.desk;
    if (desk && !acting(u"A plugin asks the person for a file"_s))
        return;
    m_plugin->busy(1);
    const auto told = [self = QPointer(this), answered](const QStringList &paths) {
        if (!self)
            return;
        QJsonArray handles;
        for (const QString &path : paths)
            if (QFileInfo(path).isFile())
                handles << self->hold({.id = 0, .path = path, .bytes = {}}, QFileInfo(path).fileName());
        QString problem;
        self->m_plugin->call(answered, {QJSValue(compact(handles))}, &problem);
        if (!problem.isEmpty())
            self->m_plugin->report(problem);
        self->m_plugin->busy(-1);
    };
    // Nobody to ask (a command): nothing picked.
    if (!desk) {
        told({});
        return;
    }
    QStringList wanted;
    for (const auto &kind : QJsonDocument::fromJson(kinds.toUtf8()).array())
        wanted << kind.toString();
    desk->pickFiles(wanted, multiple, told);
}

QString ScriptFiles::clipboardText() {
    const ScriptDesk *desk = m_plugin->m_services.desk;
    if (!allowed(u"clipboard"_s) || !desk || !acting(u"A plugin reads the clipboard"_s))
        return {};
    return desk->clipboardText();
}

bool ScriptFiles::setClipboardText(const QString &text) {
    ScriptDesk *desk = m_plugin->m_services.desk;
    if (!allowed(u"clipboard"_s) || !desk || !acting(u"A plugin writes to the clipboard"_s))
        return false;
    desk->setClipboardText(text);
    return true;
}

QString ScriptFiles::clipboardImage() {
    const ScriptDesk *desk = m_plugin->m_services.desk;
    if (!allowed(u"clipboard"_s) || !desk || !acting(u"A plugin reads the clipboard"_s))
        return u"null"_s;
    const QImage image = desk->clipboardImage();
    QByteArray png;
    QBuffer buffer(&png);
    if (image.isNull() || !buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
        return u"null"_s;
    return compact(hold({.id = 0, .path = {}, .bytes = png}, u"clipboard.png"_s));
}

bool ScriptFiles::setClipboardImage(const QString &path, const QByteArray &bytes) {
    ScriptDesk *desk = m_plugin->m_services.desk;
    if (!allowed(u"clipboard"_s) || !desk || !acting(u"A plugin writes to the clipboard"_s))
        return false;
    QImage image;
    if (!path.isEmpty()) {
        const QString real = m_plugin->inside(path, false);
        image = real.isEmpty() ? QImage() : QImage(real);
    } else {
        image = QImage::fromData(bytes);
    }
    if (image.isNull()) {
        fail(u"That is no picture Omabook can put on the clipboard: a PNG or JPEG, as bytes or a file of the plugin's folder."_s);
        return false;
    }
    desk->setClipboardImage(image);
    return true;
}

std::optional<QByteArray> ScriptFiles::contents(int id) {
    const auto held = std::ranges::find(m_held, id, &Held::id);
    if (held == m_held.end()) {
        fail(u"That file is no longer held: a plugin holds the last %1 it was given."_s.arg(heldAtMost));
        return std::nullopt;
    }
    if (held->path.isEmpty())
        return held->bytes;
    QFile file(held->path);
    const QString name = QFileInfo(held->path).fileName();
    if (file.size() > PluginFiles::maxFileBytes) {
        fail(u"\"%1\" is larger than the %2 a plugin reads at once."_s.arg(name, megabytes(PluginFiles::maxFileBytes)));
        return std::nullopt;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        fail(u"\"%1\" can't be read any more."_s.arg(name));
        return std::nullopt;
    }
    return file.readAll();
}

QJSValue ScriptFiles::heldBytes(int id) {
    const auto bytes = contents(id);
    return bytes ? m_plugin->m_engine.toScriptValue(*bytes) : QJSValue();
}

QJSValue ScriptFiles::heldBase64(int id) {
    const auto bytes = contents(id);
    return bytes ? QJSValue(QString::fromLatin1(bytes->toBase64())) : QJSValue();
}

QJSValue ScriptFiles::heldText(int id) {
    const auto bytes = contents(id);
    return bytes ? QJSValue(QString::fromUtf8(*bytes)) : QJSValue();
}

double ScriptFiles::heldCopy(int id, const QString &path) {
    const auto bytes = contents(id);
    return bytes && put(path, *bytes) ? static_cast<double>(bytes->size()) : -1;
}
