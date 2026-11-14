#pragma once

#include <QHash>
#include <QObject>
#include <QTimer>
#include <functional>

#include "library.h"
#include "metadatasource.h"

// A quick look in the catalogs for what some books are about: one question
// per book and catalog, all at once, and the first description that belongs
// to the right book wins. It answers once, with what it found, and never
// waits longer than `patience` for a catalog that doesn't answer.
class BlurbSearch : public QObject {
    Q_OBJECT

public:
    using Sources = std::function<QList<MetadataSource *>(QObject *parent)>;

    BlurbSearch(const QList<Book> &books, const Sources &sources, int patience = 6000, QObject *parent = nullptr);

    // Whether a catalog's title is the book asked for, give or take
    // punctuation, case and a subtitle.
    static bool sameTitle(const QString &asked, const QString &found);

signals:
    // Descriptions by book id; books nothing was found for are not in it.
    void finished(const QHash<qint64, QString> &blurbs);

private:
    void answered();

    QHash<qint64, QString> m_blurbs;
    QTimer m_patience;
    int m_waiting = 0;
    bool m_done = false;
};
