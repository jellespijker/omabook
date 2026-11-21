// The portal request handling follows Omacut (MIT, David Heinemeier Hansson):
// it survives a portal that stops answering or is replaced mid-dialog.
#include "portalfilepicker.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QRandomGenerator>
#include <QStandardPaths>

#include "formats.h"

namespace {

struct PortalFilterRule {
    uint type;
    QString pattern;
};
using PortalFilterRules = QList<PortalFilterRule>;

struct PortalFileFilter {
    QString name;
    PortalFilterRules rules;
};
using PortalFileFilters = QList<PortalFileFilter>;

QDBusArgument &operator<<(QDBusArgument &argument, const PortalFilterRule &rule) {
    argument.beginStructure();
    argument << rule.type << rule.pattern;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, PortalFilterRule &rule) {
    argument.beginStructure();
    argument >> rule.type >> rule.pattern;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument, const PortalFileFilter &filter) {
    argument.beginStructure();
    argument << filter.name << filter.rules;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, PortalFileFilter &filter) {
    argument.beginStructure();
    argument >> filter.name >> filter.rules;
    argument.endStructure();
    return argument;
}

void registerPortalFilterTypes() {
    static const bool registered = [] {
        qDBusRegisterMetaType<PortalFilterRule>();
        qDBusRegisterMetaType<PortalFilterRules>();
        qDBusRegisterMetaType<PortalFileFilter>();
        qDBusRegisterMetaType<PortalFileFilters>();
        return true;
    }();
    Q_UNUSED(registered);
}

PortalFileFilter bookFilter() {
    PortalFilterRules rules;
    for (const QString &extension : Formats::extensions())
        rules.append({0, "*." + extension});
    // Archives too: books are passed around packed, and a zip of Markdown
    // files is a book in itself.
    for (const char *archive : {"zip", "rar", "7z", "tar", "tgz"})
        rules.append({0, QStringLiteral("*.") + archive});
    return {PortalFilePicker::tr("Books"), rules};
}

PortalFileFilter pictureFilter() {
    return {PortalFilePicker::tr("Pictures"), {{1, QStringLiteral("image/*")}}};
}

PortalFileFilter pluginFilter() {
    return {PortalFilePicker::tr("Plugins"), {{0, QStringLiteral("*.zip")}}};
}

PortalFileFilter spaceFilter() { return {PortalFilePicker::tr("Shared collections"), {{0, QStringLiteral("*.omabook-space")}}}; }

// What a plugin named (FilePicker::pickForPlugin): kinds by their MIME
// types, extensions by their names; none when it named nothing.
PortalFileFilter kindsFilter(const QStringList &kinds) {
    static const QHash<QString, QString> types{{QStringLiteral("image"), QStringLiteral("image/*")},
                                               {QStringLiteral("audio"), QStringLiteral("audio/*")},
                                               {QStringLiteral("video"), QStringLiteral("video/*")},
                                               {QStringLiteral("text"), QStringLiteral("text/*")},
                                               {QStringLiteral("pdf"), QStringLiteral("application/pdf")}};
    PortalFilterRules rules;
    for (const QString &kind : kinds) {
        if (kind.startsWith(u'.') && kind.size() > 1)
            rules.append({0, u'*' + kind});
        else if (types.contains(kind) || kind.contains(u'/'))
            rules.append({1, types.value(kind, kind)});
    }
    return {PortalFilePicker::tr("The files it takes"), rules};
}

PortalFileFilter allFilter() {
    return {PortalFilePicker::tr("All files"), {{0, QStringLiteral("*")}}};
}

QString portalToken() { return QStringLiteral("omabook_%1").arg(QRandomGenerator::global()->generate()); }

QByteArray portalPathBytes(const QString &path) {
    QByteArray bytes = path.toUtf8();
    bytes.append('\0');
    return bytes;
}

QString startFolder(QStandardPaths::StandardLocation location) {
    const QString folder = QStandardPaths::writableLocation(location);
    return QDir(folder).exists() ? folder : QDir::homePath();
}

// A save dialog's options: the button, and the folder and name of the file suggested.
void saveAs(QVariantMap &options, const QString &suggested) {
    const QFileInfo target(suggested);
    options.insert(QStringLiteral("accept_label"), PortalFilePicker::tr("Save"));
    options.insert(QStringLiteral("current_folder"), portalPathBytes(target.absolutePath()));
    options.insert(QStringLiteral("current_name"), target.fileName());
}

} // namespace

PortalFilePicker::PortalFilePicker(QObject *parent) : FilePicker(parent) { registerPortalFilterTypes(); }

void PortalFilePicker::pick(Purpose purpose, const QString &suggested) {
    QVariantMap options;
    options.insert(QStringLiteral("modal"), true);
    switch (purpose) {
    case AddBooks:
        options.insert(QStringLiteral("accept_label"), tr("Add"));
        options.insert(QStringLiteral("multiple"), true);
        options.insert(QStringLiteral("current_folder"), portalPathBytes(startFolder(QStandardPaths::DownloadLocation)));
        options.insert(QStringLiteral("filters"), QVariant::fromValue(PortalFileFilters{bookFilter(), allFilter()}));
        options.insert(QStringLiteral("current_filter"), QVariant::fromValue(bookFilter()));
        requestFile(QStringLiteral("OpenFile"), tr("Add Books"), options, purpose);
        break;
    case AddFolder:
        options.insert(QStringLiteral("accept_label"), tr("Add"));
        options.insert(QStringLiteral("directory"), true);
        options.insert(QStringLiteral("current_folder"), portalPathBytes(QDir::homePath()));
        requestFile(QStringLiteral("OpenFile"), tr("Add a Folder of Books"), options, purpose);
        break;
    case ChooseLibrary:
        options.insert(QStringLiteral("accept_label"), tr("Use"));
        options.insert(QStringLiteral("directory"), true);
        options.insert(QStringLiteral("current_folder"), portalPathBytes(QDir::homePath()));
        requestFile(QStringLiteral("OpenFile"), tr("Choose a Library Folder"), options, purpose);
        break;
    case ChooseCover:
        options.insert(QStringLiteral("accept_label"), tr("Use"));
        options.insert(QStringLiteral("current_folder"), portalPathBytes(startFolder(QStandardPaths::PicturesLocation)));
        options.insert(QStringLiteral("filters"), QVariant::fromValue(PortalFileFilters{pictureFilter(), allFilter()}));
        options.insert(QStringLiteral("current_filter"), QVariant::fromValue(pictureFilter()));
        requestFile(QStringLiteral("OpenFile"), tr("Choose a Cover"), options, purpose);
        break;
    case ExportBook:
        saveAs(options, suggested);
        requestFile(QStringLiteral("SaveFile"), tr("Save a Copy"), options, purpose);
        break;
    case ImportReading:
        // The file a reading site lets its users download of their shelves.
        options.insert(QStringLiteral("accept_label"), tr("Import"));
        options.insert(QStringLiteral("current_folder"), portalPathBytes(startFolder(QStandardPaths::DownloadLocation)));
        requestFile(QStringLiteral("OpenFile"), tr("Import Your Shelves"), options, purpose);
        break;
    case ExportReading:
        saveAs(options, suggested);
        requestFile(QStringLiteral("SaveFile"), tr("Save Your Reading"), options, purpose);
        break;
    case InstallPlugin:
        options.insert(QStringLiteral("accept_label"), tr("Choose"));
        options.insert(QStringLiteral("current_folder"), portalPathBytes(startFolder(QStandardPaths::DownloadLocation)));
        options.insert(QStringLiteral("filters"), QVariant::fromValue(PortalFileFilters{pluginFilter(), allFilter()}));
        options.insert(QStringLiteral("current_filter"), QVariant::fromValue(pluginFilter()));
        requestFile(QStringLiteral("OpenFile"), tr("Install a Plugin"), options, purpose);
        break;
    case InstallPluginFolder:
        options.insert(QStringLiteral("accept_label"), tr("Choose"));
        options.insert(QStringLiteral("directory"), true);
        options.insert(QStringLiteral("current_folder"), portalPathBytes(QDir::homePath()));
        requestFile(QStringLiteral("OpenFile"), tr("Install a Plugin From a Folder"), options, purpose);
        break;
    case SaveSpace:
        saveAs(options, suggested);
        requestFile(QStringLiteral("SaveFile"), tr("Save the Shared Collection"), options, purpose);
        break;
    case OpenSpace:
        options.insert(QStringLiteral("accept_label"), tr("Open"));
        options.insert(QStringLiteral("current_folder"), portalPathBytes(startFolder(QStandardPaths::DownloadLocation)));
        options.insert(QStringLiteral("filters"), QVariant::fromValue(PortalFileFilters{spaceFilter(), allFilter()}));
        options.insert(QStringLiteral("current_filter"), QVariant::fromValue(spaceFilter()));
        requestFile(QStringLiteral("OpenFile"), tr("Subscribe to a Shared Collection"), options, purpose);
        break;
    case PluginFiles:
        pickForPlugin({}, false);
        break;
    case PluginSave:
        saveForPlugin(suggested, {});
        break;
    }
}

void PortalFilePicker::pickForPlugin(const QStringList &kinds, bool multiple) {
    QVariantMap options;
    options.insert(QStringLiteral("modal"), true);
    options.insert(QStringLiteral("accept_label"), tr("Choose"));
    options.insert(QStringLiteral("multiple"), multiple);
    options.insert(QStringLiteral("current_folder"), portalPathBytes(QDir::homePath()));
    if (const PortalFileFilter wanted = kindsFilter(kinds); !wanted.rules.isEmpty()) {
        options.insert(QStringLiteral("filters"), QVariant::fromValue(PortalFileFilters{wanted, allFilter()}));
        options.insert(QStringLiteral("current_filter"), QVariant::fromValue(wanted));
    }
    requestFile(QStringLiteral("OpenFile"), multiple ? tr("Choose Files") : tr("Choose a File"), options, PluginFiles);
}

void PortalFilePicker::saveForPlugin(const QString &name, const QStringList &kinds) {
    QVariantMap options;
    options.insert(QStringLiteral("modal"), true);
    // Only the name is the plugin's to say; the folder is the person's Documents to start with.
    saveAs(options, QDir(startFolder(QStandardPaths::DocumentsLocation)).filePath(QFileInfo(name).fileName()));
    if (const PortalFileFilter wanted = kindsFilter(kinds); !wanted.rules.isEmpty()) {
        options.insert(QStringLiteral("filters"), QVariant::fromValue(PortalFileFilters{wanted, allFilter()}));
        options.insert(QStringLiteral("current_filter"), QVariant::fromValue(wanted));
    }
    requestFile(QStringLiteral("SaveFile"), tr("Save a File"), options, PluginSave);
}

bool PortalFilePicker::connectToRequestPath(const QString &path) {
    m_pendingPath = path;
    return QDBusConnection::sessionBus().connect(
        QStringLiteral("org.freedesktop.portal.Desktop"), m_pendingPath, QStringLiteral("org.freedesktop.portal.Request"),
        QStringLiteral("Response"), this, SLOT(handleResponse(uint, QVariantMap)));
}

void PortalFilePicker::closePendingRequest() {
    if (m_pendingPath.isEmpty())
        return;
    QDBusInterface request(QStringLiteral("org.freedesktop.portal.Desktop"), m_pendingPath,
                           QStringLiteral("org.freedesktop.portal.Request"), QDBusConnection::sessionBus());
    request.asyncCall(QStringLiteral("Close"));
}

void PortalFilePicker::requestFile(const QString &method, const QString &title, QVariantMap options, Purpose purpose) {
    // A request whose Response never arrives (the portal crashed or was
    // killed with the dialog up) must not wedge every later dialog behind
    // it: close the stuck one best-effort and replace it.
    if (m_pending) {
        closePendingRequest();
        clearPending();
    }

    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusInterface portal(QStringLiteral("org.freedesktop.portal.Desktop"), QStringLiteral("/org/freedesktop/portal/desktop"),
                          QStringLiteral("org.freedesktop.portal.FileChooser"), bus);
    if (!portal.isValid()) {
        emit failed(tr("The desktop's file dialog is not available."));
        return;
    }

    // Subscribe to the Response signal at the request path the portal will
    // derive from our handle_token *before* making the call, so a response
    // can't slip past while our match rule is still being installed.
    const QString token = portalToken();
    options.insert(QStringLiteral("handle_token"), token);
    QString sender = bus.baseService().mid(1);
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    const QString predictedPath = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);

    m_pending = true;
    m_pendingPurpose = purpose;
    if (!connectToRequestPath(predictedPath)) {
        clearPending();
        emit failed(tr("The desktop's file dialog is not answering."));
        return;
    }

    const int generation = ++m_pendingGeneration;
    auto *watcher = new QDBusPendingCallWatcher(portal.asyncCall(method, QString(), title, options), this);
    m_pendingWatcher = watcher;

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, generation]() {
        QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        watcher->deleteLater();
        if (m_pendingWatcher == watcher)
            m_pendingWatcher = nullptr;

        // Superseded by a newer request, or a response may already have
        // been handled while the reply was in flight.
        if (generation != m_pendingGeneration || !m_pending)
            return;

        if (reply.isError()) {
            clearPending();
            emit failed(tr("The desktop's file dialog failed: %1").arg(reply.error().message()));
            return;
        }

        // Old portal versions can hand back a different request path than the
        // handle_token predicts; move our subscription over if so.
        const QString actualPath = reply.value().path();
        if (actualPath != m_pendingPath) {
            QDBusConnection::sessionBus().disconnect(
                QStringLiteral("org.freedesktop.portal.Desktop"), m_pendingPath,
                QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"), this,
                SLOT(handleResponse(uint, QVariantMap)));
            if (!connectToRequestPath(actualPath)) {
                clearPending();
                emit failed(tr("The desktop's file dialog is not answering."));
            }
        }
    });
}

void PortalFilePicker::handleResponse(uint response, const QVariantMap &results) {
    const Purpose purpose = m_pendingPurpose;
    clearPending();
    QList<QUrl> urls;
    for (const QString &uri : results.value(QStringLiteral("uris")).toStringList())
        urls << QUrl(uri);
    if (response == 0 && !urls.isEmpty())
        emit picked(purpose, urls);
    else
        emit dismissed(purpose);
}

void PortalFilePicker::clearPending() {
    // A caller that never gets its reply must not outlive the request it
    // belonged to (repeated stuck requests would otherwise pile them up).
    if (m_pendingWatcher) {
        m_pendingWatcher->deleteLater();
        m_pendingWatcher = nullptr;
    }
    if (!m_pendingPath.isEmpty()) {
        QDBusConnection::sessionBus().disconnect(
            QStringLiteral("org.freedesktop.portal.Desktop"), m_pendingPath, QStringLiteral("org.freedesktop.portal.Request"),
            QStringLiteral("Response"), this, SLOT(handleResponse(uint, QVariantMap)));
    }
    m_pendingPath.clear();
    m_pending = false;
}
