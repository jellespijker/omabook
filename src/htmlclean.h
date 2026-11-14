#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <functional>

// A book's chapters are web pages, written for a browser and often not well
// formed. Omabook sets every book in the reader's own font and theme, so what
// it needs from a chapter is its structure: headings, paragraphs, emphasis,
// lists, tables, pictures and links. These functions read HTML forgivingly
// and write that structure back as the subset Qt's rich text lays out.

struct HtmlToken {
    enum Type { Text, Open, Close };
    Type type = Text;
    QString name;                    // lower case, without a namespace prefix
    QHash<QString, QString> attributes; // names lower case, without a prefix
    QString text;                    // for Text, as written, entities and all
    bool selfClosing = false;
};

QList<HtmlToken> tokenizeHtml(const QString &html);

// What a stylesheet says about a class, as far as rich text can show it.
enum TextStyle { Italic = 1, Bold = 2, Center = 4, Right = 8 };
// Class name to TextStyle flags, from a book's stylesheets.
QHash<QString, int> classStyles(const QString &css);

struct CleanOptions {
    // The attributes for a picture (`src="…" width="…"`), or empty to drop it.
    std::function<QString(const QString &source)> image;
    // Where a link goes in the reader, or empty to leave only its text.
    std::function<QString(const QString &target)> link;
    QHash<QString, int> styles;
};

QString cleanHtml(const QString &html, const CleanOptions &options = {});
// The words of some HTML, one paragraph per line.
QString htmlToText(const QString &html);
// Bytes to text by their byte order mark, their own declaration (XML or HTML
// meta), UTF-8 when valid, and Windows-1252 as the last resort.
QString decodeText(const QByteArray &bytes);
