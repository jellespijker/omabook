#include "portholder.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <algorithm>
#include <tuple>

using namespace Qt::StringLiterals;

namespace {

// The inodes of the sockets listening on `port`, from a net/tcp table: its
// columns are the slot, the local address as hex "address:port", the remote
// one, the state (0A is listening), and further on the inode.
[[nodiscard]] QStringList listening(const QString &table, quint16 port) {
    QFile file(table);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QString wanted = u":%1"_s.arg(port, 4, 16, QChar('0')).toUpper();
    QStringList inodes;
    file.readLine(); // the heading
    while (!file.atEnd()) {
        const QStringList columns = QString::fromLatin1(file.readLine()).split(' ', Qt::SkipEmptyParts);
        if (columns.size() > 9 && columns.at(1).endsWith(wanted) && columns.at(3) == u"0A")
            inodes << columns.at(9);
    }
    return inodes;
}

} // namespace

QString PortHolder::program(quint16 port, const QString &proc) {
    QStringList inodes = listening(proc + u"/net/tcp"_s, port) + listening(proc + u"/net/tcp6"_s, port);
    inodes.removeAll(u"0"_s);
    if (inodes.isEmpty())
        return {};
    const QDir root(proc);
    for (const QString &pid : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool number = false;
        std::ignore = pid.toLongLong(&number); // only whether it is a process's folder
        if (!number)
            continue;
        const QDir fds(root.filePath(pid + u"/fd"_s));
        for (const QString &fd : fds.entryList(QDir::System | QDir::Files | QDir::NoDotAndDotDot)) {
            const QString target = QFileInfo(fds.filePath(fd)).symLinkTarget();
            const QString name = QFileInfo(target).fileName();
            if (!std::ranges::any_of(inodes, [&name](const QString &inode) { return name == u"socket:[%1]"_s.arg(inode); }))
                continue;
            QFile comm(root.filePath(pid + u"/comm"_s));
            if (comm.open(QIODevice::ReadOnly))
                return QString::fromUtf8(comm.readAll()).trimmed();
            return {};
        }
    }
    return {};
}
