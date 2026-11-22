#pragma once

#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <QtTypes>

class ChronologyBrowser;
class Library;

// The branches the AI proposed (#171, chronologybranch.h) in what one
// chronology view shows, for the reader to take or not: an alternate
// history, an alternative universe, a book read as departing from another.
// Only of one's own graph: what someone shared is not one's to decide.
// Like the rest of the view, only as far as the reader read: a split they
// haven't reached is not proposed yet.
class ChronologyProposals : public QObject {
    Q_OBJECT
    // The ChronologyBrowser whose reading it lists from.
    Q_PROPERTY(QObject *browser READ browser WRITE setBrowser NOTIFY browserChanged)
    // [{timeline, name, from, at}]: the branch, and what and where it branches off.
    Q_PROPERTY(QVariantList list READ list NOTIFY changed)

public:
    explicit ChronologyProposals(QObject *parent = nullptr) : QObject(parent) {}

    QObject *browser() const;
    void setBrowser(QObject *browser);
    // Tests work on a library of their own, without a backend.
    void setLibrary(Library *library) { m_library = library; }
    QVariantList list() const { return m_list; }

    // The reader's word: the branch is taken, a lane beside what it branches
    // off, or refused, its events left in their book's own order. False when
    // it could not be kept.
    Q_INVOKABLE bool decide(qint64 timeline, bool accept);

signals:
    void browserChanged();
    void changed();

private:
    void read();
    [[nodiscard]] Library *library() const;

    QPointer<ChronologyBrowser> m_browser;
    Library *m_library = nullptr;
    QVariantList m_list;
};
