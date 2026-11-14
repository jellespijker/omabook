#pragma once

#include <QList>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QString>
#include <QStringList>

#include "calibreplugin.h"
#include "metadatasource.h"

class Settings;

// A plugin, as Calibre thinks of one: something of a known kind with a name,
// a version and an author, that can be turned on and off. Omabook has three
// origins for them, listed and switched the same way:
//
//   built-in   the sources compiled into Omabook
//   calibre    Calibre's own plugins, found by the Calibre plugin
//              (calibreplugin.h): its metadata sources, run inside Calibre,
//              and whatever else is installed there, listed under the kind
//              "calibre" beneath the Calibre plugin itself (id "calibre"),
//              which switches all of it
//   folder     a program in ~/.config/omabook/plugins/<name>/ with a
//              plugin.json beside it, in any language; or a script there,
//              run inside Omabook (scriptplugins.h), listed under the kind
//              "script" because one script may add several things at once
//
// The kinds follow the ones Calibre sorts its plugins into:
//
//   metadata    a source of book details (Calibre: "Metadata source")
//   format      reads a format (Calibre: "Metadata reader", "Conversion input")
//   conversion  turns one format into another (Calibre: "Conversion input/output")
//   import      something done as books come in (Calibre: "File type")
//   catalog     writes the library out as one file (Calibre: "Catalog generator")
//   reading     a site where the reading is kept, brought in step with the library
//   ai          a language model (Calibre: "AI provider")
//
// Only metadata plugins can be added from outside and switched off; the rest
// are what Omabook is made of, listed so it is plain what it can do.
struct PluginInfo {
    QString id;             // "builtin:Open Library", "calibre:Amazon.com", "folder:goodreads"
    QString name;
    QString kind = "metadata";
    QString origin;         // "built-in", "calibre" or "folder"
    QString version;
    QString author;
    QString description;
    QStringList fields;     // what it can bring, by field key
    bool enabled = true;
    bool switchable = true; // whether it can be turned off (what Omabook is made of can't)
    QString problem;        // why it can't be used as it is, if it can't
    QString needs;          // folder plugins: that it asks for a newer Omabook than this, in words
    QString folder;         // folder plugins: where it lives
    QStringList command;    // folder plugins: the program and its arguments
    QString script;         // script plugins: the script's file name
    QStringList permissions; // script plugins: what plugin.json asks to be allowed ("http", "files", "run")
};

class Plugins {
public:
    // Tests pass their own plugin folder and stand-in for calibre-debug
    // ("-" for no Calibre at all).
    explicit Plugins(Settings *settings, const QString &folder = {}, const QString &calibreDebug = {});

    QString folder() const { return m_folder; }
    // Every plugin there is. Calibre's are asked for once (a third of a
    // second) and remembered on disk; `refresh` asks again.
    QList<PluginInfo> list(bool refresh = false) const;
    // Without asking Calibre: what is known so far. For the GUI thread.
    QList<PluginInfo> known() const;
    bool setEnabled(const QString &id, bool enabled);
    // The sources of book details are asked in a set order, which the user
    // can change: the ids of every metadata plugin as they are asked now,
    // one of them moved earlier (`by` below zero) or later, or all of them
    // put in the order given. Kept in the settings; false for an id that is
    // no source or a move off either end.
    QStringList order() const;
    bool move(const QString &id, int by);
    bool setOrder(const QStringList &ids);
    // Everything Calibre: what the rest of the app asks instead of Calibre's pieces.
    CalibrePlugin &calibre() { return m_calibre; }
    const CalibrePlugin &calibre() const noexcept { return m_calibre; }

    // A source for each plugin that is on and usable, owned by `parent`.
    QList<MetadataSource *> sources(const QList<PluginInfo> &plugins, QNetworkAccessManager *network, QObject *parent) const;

private:
    QList<PluginInfo> builtIn() const;
    QList<PluginInfo> fromFolder() const;
    void decide(QList<PluginInfo> *plugins) const;

    Settings *m_settings;
    QString m_folder;
    CalibrePlugin m_calibre;
};
