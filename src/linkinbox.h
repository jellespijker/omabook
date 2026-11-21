#pragma once

#include <QLocalServer>
#include <QObject>
#include <QString>

class QLocalSocket;

// Where the window already open hears of a link it is to open (#138): a
// link to a place in a book (BookLink) clicked anywhere on the desktop
// starts `omabook open <link>`, which hands it over here and is done,
// rather than opening a second window. A local socket of the person's own,
// in their runtime folder; the first window to listen on it takes the links.
class LinkInbox : public QObject {
    Q_OBJECT

public:
    // `name` is the socket's: the one a person's windows share unless a test gives its own.
    explicit LinkInbox(QString name = sharedName(), QObject *parent = nullptr);

    [[nodiscard]] static QString sharedName();
    // Hands a link to the window that listens at `name`: whether one took it.
    [[nodiscard]] static bool handOver(const QString &link, const QString &name = sharedName());

    // Listens for links, unless another window already does: whether this one does.
    bool listen();

signals:
    void linked(const QString &link);

private:
    void read(QLocalSocket &socket);

    QString m_name;
    QLocalServer m_server;
};
