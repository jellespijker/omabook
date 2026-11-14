#pragma once

#include <QColor>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QVariantMap>

class Reader;
class Settings;

// The reading format for the window: what is in force for the open book,
// the defaults and the book's own, and the chapter set with them. It
// changes them too, as the "Aa" panel and the settings ask.
class PageFormat : public QObject {
    Q_OBJECT
    // The options in force, by key: the defaults with the open book's own over them.
    Q_PROPERTY(QVariantMap effective READ effective NOTIFY changed)
    Q_PROPERTY(QVariantMap defaults READ defaults NOTIFY changed)
    // Only the open book's own options; empty when it follows the defaults.
    Q_PROPERTY(QVariantMap own READ own NOTIFY changed)
    // Whether a book is open for options of its own to be given to.
    Q_PROPERTY(bool hasBook READ hasBook NOTIFY changed)
    Q_PROPERTY(QVariantMap bounds READ bounds CONSTANT)
    Q_PROPERTY(QStringList fonts READ fonts CONSTANT)
    // The chapter in front of the reader, set with the options in force.
    Q_PROPERTY(QString html READ html NOTIFY htmlChanged)
    // The color links are shown in, which is the theme's.
    Q_PROPERTY(QColor linkColor READ linkColor WRITE setLinkColor NOTIFY htmlChanged)

public:
    PageFormat(Reader *reader, Settings *settings, QObject *parent = nullptr);

    QVariantMap effective() const { return m_effective.toVariantMap(); }
    QVariantMap defaults() const { return m_defaults.toVariantMap(); }
    QVariantMap own() const { return m_own.toVariantMap(); }
    bool hasBook() const;
    QVariantMap bounds() const;
    // The fonts there are, by family, for the font picker.
    QStringList fonts() const;
    QString html() const noexcept { return m_html; }
    QColor linkColor() const noexcept { return m_links; }
    void setLinkColor(const QColor &color);

    // Changes one option, for the open book alone or for the defaults; what
    // is wrong with the value when it can't be, else empty.
    Q_INVOKABLE QString set(const QString &key, const QVariant &value, bool forBook);
    // The open book follows the defaults again.
    Q_INVOKABLE void resetBook();
    // The defaults and the book's own as they are, and back to that: what
    // the panel's Reset puts back.
    Q_INVOKABLE QVariantMap snapshot() const;
    Q_INVOKABLE void restore(const QVariantMap &snapshot);
    // Larger or smaller type, where the size is: the book's own size when
    // it has one, else the default.
    Q_INVOKABLE void zoom(int steps);

    // Reads the defaults and the open book's own again.
    void refresh();

signals:
    void changed();
    void htmlChanged();

private:
    void restyle();

    Reader *m_reader;
    Settings *m_settings;
    QJsonObject m_defaults, m_own, m_effective;
    QString m_html, m_chapter;
    QColor m_links{"#7aa2f7"};
};
