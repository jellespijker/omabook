#pragma once

#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QString>
#include <QStringList>

#include "importer.h"

class MetadataSource;
class QObject;
class Settings;
struct PluginInfo;

// A Calibre library found on this computer, and what was answered when it
// was offered for import.
struct CalibreLibrary {
    QString path;
    QString name;           // the folder's name
    int books = 0;          // how many books Calibre keeps there; -1 when it can't be read
    QString answer;         // "imported", "later", "never", or empty when it was never offered
    qint64 answeredAt = 0;  // seconds since the epoch

    QJsonObject toJson() const;
};

// One of Calibre's plugins: one it ships with, or one installed there.
struct CalibreAddOn {
    QString name;
    QString type;           // Calibre's own word for its kind: "Metadata source", "File type", "Device interface", ...
    QString kind;           // Omabook's: "metadata", "conversion", "import", "format", "catalog", "ai", "device",
                            // "interface", "store" or "other"
    QString version;
    QString author;
    QString description;
    QStringList fields;     // metadata sources: what it can bring, by Omabook's field keys
    bool installed = false; // added to Calibre by the user, rather than shipped with it
    QString file;           // installed ones: the plugin's zip
    bool enabledInCalibre = true;
    bool usable = false;    // whether Omabook can use it
    QString note;           // why it can't be used, or how it is; English

    QJsonObject toJson() const;
};

// Everything Omabook does with Calibre, as one plugin that can be switched
// off: reading Calibre's libraries (with what Calibre knows of the reading),
// running its metadata source plugins, converting with ebook-convert, and
// finding Calibre's libraries and plugins on this computer. The rest of the
// app asks this class rather than Calibre's pieces, so that switched off,
// nothing of Calibre's is run, read, offered or shown.
//
// It is on by default when Calibre or a Calibre library is found; the user's
// own choice, kept in the settings under the plugin id "calibre", wins.
// It can be asked from any thread.
class CalibrePlugin {
public:
    static const QString id; // "calibre"

    // Where Calibre is. Tests fill this in themselves, so they never look at
    // the real home; an empty field is something that isn't there.
    struct Where {
        QString calibreDebug;   // the program that runs Calibre's plugins
        QString ebookConvert;   // the converter
        QString configFolder;   // Calibre's own settings, with its plugins folder
        QString defaultLibrary; // where Calibre puts a first library
        QString cacheFolder;    // where the bridge and the list of plugins are kept

        // As installed: programs on the PATH, CALIBRE_CONFIG_DIRECTORY or
        // ~/.config/calibre, and ~/Calibre Library.
        static Where onThisComputer();
        // For tests of the rest of the app: a stand-in calibre-debug ("-" for
        // none) and nothing else, except a config folder named by
        // CALIBRE_CONFIG_DIRECTORY, which is how such a test brings its own.
        static Where standIn(const QString &calibreDebug);
    };

    // `calibreDebug` empty means Calibre as installed; anything else is a
    // test's stand-in (see Where::standIn).
    explicit CalibrePlugin(Settings *settings, const QString &calibreDebug = {});
    CalibrePlugin(Settings *settings, const Where &where);

    // Whether Calibre's programs are installed, and whether there is anything
    // of Calibre's here at all: its programs or a library.
    bool installed() const;
    bool found() const;
    // Whether the plugin is on, and whether it was the user who turned it off.
    bool enabled() const;
    bool switchedOff() const;
    // Whether a Calibre library is on this computer, on or off: what the
    // plugin manager asks to say that switching it off left something unused.
    bool hasLibraries() const { return !libraryFolders().isEmpty(); }
    void setEnabled(bool enabled);

    // --- Libraries ---

    // Calibre's libraries on this computer: the ones Calibre's settings name
    // and its default folder, each holding a metadata.db. Empty when off.
    QList<CalibreLibrary> libraries() const;
    // The ones to ask about: never offered, or put off more than a week ago.
    // With `asked` (the user went looking), every one that wasn't imported or
    // refused for good.
    QList<CalibreLibrary> librariesToOffer(bool asked = false) const;
    // Remembers "imported", "later" or "never" for a library; "" forgets.
    void answer(const QString &library, const QString &answer);
    // Whether an import reads this folder as a Calibre library. A folder the
    // user points at counts as a library found, so only switching the plugin
    // off makes it a plain folder of books.
    bool reads(const QString &folder) const;
    ImportOptions importOptions() const;

    // --- Calibre's plugins ---

    // Every plugin of Calibre's: its metadata sources (asked of Calibre
    // itself) and whatever is installed in its plugins folder, of any kind.
    // The answer is kept on disk, so Calibre is only started when something
    // changed (Calibre was updated, a plugin was added) or with `refresh`.
    // Empty when off.
    QList<CalibreAddOn> addOns(bool refresh = false) const;
    // Without starting Calibre: what was found last time. For the GUI thread.
    QList<CalibreAddOn> knownAddOns() const;
    // The rows of the plugin registry: the Calibre plugin itself, what it
    // does (conversion, importing libraries), and beneath it each of
    // Calibre's plugins, from knownAddOns().
    QList<PluginInfo> entries() const;
    // A look-up source for one of the registry's metadata rows, owned by
    // `parent`; null when the plugin is off or Calibre isn't installed.
    MetadataSource *source(const PluginInfo &plugin, QObject *parent) const;

    // --- Conversion ---

    // ebook-convert, or empty when it isn't installed or the plugin is off.
    QString converter() const;
    // Why there is no converter, in English; empty when there is one.
    QString converterProblem() const;

private:
    QStringList libraryFolders() const;
    QString bridge() const;
    QString stamp() const;
    QString cacheFile() const;
    QList<CalibreAddOn> discover() const;

    Settings *m_settings;
    Where m_where;
    mutable QMutex m_lock;
    mutable bool m_looked = false;
    mutable bool m_anyLibrary = false;
    mutable bool m_loaded = false;
    mutable QString m_stamp;
    mutable QList<CalibreAddOn> m_addOns;
};
