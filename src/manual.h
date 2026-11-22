#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

class Customization;
class Settings;

// What Omabook says about itself: the help's pages, the feature tour, and
// what is new in a release. All of it is data, written once and used by the
// window (?) and the command line (`omabook help`) alike:
//
//   helptopics.cpp   the pages of the help, as text
//   tour.cpp         the first-use tour, a step at a time
//   whatsnew.cpp     the features of each release; a new one is one entry
//
// The keys are never written into that text. A page names a command in
// braces ("{library.add} adds books") and the key shown is whichever key
// keys.json gives it now, so help can't disagree with the keyboard.
class Manual : public QObject {
    Q_OBJECT
    // Every command with its keys as they are bound now, each
    // {id, place, keys, key, text, short}: place is "global", "library",
    // "reader", "graph" or "plugins", keys all of them ("Space  PgDown"),
    // key the first, and short the text up to its first colon, semicolon
    // or bracket, for a menu. A plugin's command also has `plugin`, the
    // plugin's name, and `where`, the place it works in ("global",
    // "library", "reader").
    Q_PROPERTY(QVariantList commands READ commands NOTIFY changed)
    // The keys that belong to one panel or dialog and only work there (the
    // menu beside selected words, the contents, the Aa panel, the plugin
    // manager...), which keys.json doesn't change: each
    // {id, title, keys: [{keys, text}]}. Defined in panelkeys.cpp.
    Q_PROPERTY(QVariantList panelKeys READ panelKeys NOTIFY changed)
    // The pages, each {id, title, summary, sections: [{heading, html}],
    // commands: [ids the page names], search: its words in lower case}.
    Q_PROPERTY(QVariantList topics READ topics NOTIFY changed)
    Q_PROPERTY(QString version READ version CONSTANT)
    // What may be offered, quietly, at the bottom of the window: the tour on
    // a first start, its second half in the first book opened after the
    // first was taken, and what is new once the version has moved on.
    Q_PROPERTY(bool tourOffered READ tourOffered NOTIFY changed)
    Q_PROPERTY(bool readerTourOffered READ readerTourOffered NOTIFY changed)
    Q_PROPERTY(bool newsOffered READ newsOffered NOTIFY changed)

public:
    struct Section {
        QString heading;
        QString text;
    };
    struct Topic {
        QString id;
        QString title;
        QString summary;
        QList<Section> sections;
    };
    // One card of a tour. `spot` is the objectName of what it points at in
    // the window (several, separated by |, take the first that shows; none
    // puts the card in the middle), `command` a command whose key the card
    // shows, `place` where the step belongs ("library", "reader" or
    // anywhere), and `when` narrows it further: "empty" or "books" for the
    // library having none or some, "touch" for a touchscreen.
    struct Step {
        QString id;
        QString version;
        QString title;
        QString text;
        QString spot;
        QString command;
        QString place;
        QString when;
    };
    // The text itself, in the language in use. Defined in helptopics.cpp,
    // tour.cpp and whatsnew.cpp.
    static QList<Topic> topicList();
    // A panel or dialog and the keys of its own, as `panelKeys` gives them.
    struct PanelKey {
        QString keys;
        QString text;
    };
    struct Panel {
        QString id;
        QString title;
        QList<PanelKey> keys;
    };
    static QList<Panel> panelList();
    static QList<Step> tourSteps();
    static QList<Step> features();

    // `settings` may be null where nothing is remembered (the command line).
    // Tests pass a version of their own.
    Manual(Settings *settings, Customization *custom, const QString &version = {}, QObject *parent = nullptr);

    QVariantList commands() const;
    QVariantList panelKeys() const;
    QVariantList topics() const;
    QString version() const noexcept { return m_version; }
    bool tourOffered() const;
    bool readerTourOffered() const;
    bool newsOffered() const;

    // The first-use tour for where the window is: the shelf (with or without
    // books on it) or the open book.
    Q_INVOKABLE QVariantList tour(const QString &place, bool empty, bool touch) const;
    // What arrived since the version last seen; with `all`, everything the
    // newest release brought, for whoever asks for it again.
    Q_INVOKABLE QVariantList news(bool all = false) const;
    // The tour was taken to its end in that place, or turned down or left.
    // Either way it is never offered again.
    Q_INVOKABLE void tourAnswered(const QString &place, bool finished);
    // What is new was shown, or turned down.
    Q_INVOKABLE void newsAnswered();

    // For the command line: one page as text, the list of pages, and the
    // keys, each also as JSON. An empty answer means there is no such page.
    QString topicText(const QString &id) const;
    QJsonObject topicJson(const QString &id) const;
    QString overviewText() const;
    QJsonObject overviewJson() const;
    QString keysText() const;
    QJsonObject keysJson() const;

    // The commands a text names in braces, in the order it names them.
    static QStringList mentioned(const QString &text);

signals:
    void changed();

private:
    QString firstKey(const QString &command) const;
    // A text with its {commands} as keys: for a terminal, and as the rich
    // text the window shows (keys bold, `code` in a fixed-width face).
    QString plain(const QString &text) const;
    QString html(const QString &text) const;
    QVariantMap card(const Step &step) const;
    // The panels' keys, in two lists of a size to read (panelkeys.cpp).
    static QList<Panel> readingPanels();
    static QList<Panel> toolPanels();

    Settings *m_settings;
    Customization *m_custom;
    QString m_version;
};
