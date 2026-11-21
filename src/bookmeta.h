#pragma once

#include <QImage>
#include <QJsonObject>
#include <QString>
#include <QStringList>

// What is known about a book, whatever told us: the file itself, an online
// catalog, the AI, or the user.
struct BookMeta {
    QString title;
    QStringList authors;
    QString series;
    double seriesIndex = 0;
    QString publisher;
    QString language;
    QString isbn;
    QString published;
    QString description;
    QStringList tags;
    QImage cover;

    QString authorText() const { return authors.join(" & "); }
    static QStringList splitAuthors(const QString &text);
    static QStringList splitTags(const QString &text);

    // Takes each field `other` has a value for, leaving the rest as they are.
    void apply(const BookMeta &other);
    // Takes from `other` only what is still empty here.
    void fillFrom(const BookMeta &other);

    QJsonObject toJson() const;
    static BookMeta fromJson(const QJsonObject &object);
    // Takes each field `object` names (as toJson() names them), even an empty
    // one, leaving the fields it doesn't name as they are (#154).
    void applyJson(const QJsonObject &object);
};
