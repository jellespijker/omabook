#include "hubstore.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <array>
#include <optional>
#include <tuple>

#include "hubcomputers.h"
#include "log.h"

using namespace Qt::StringLiterals;

namespace {

constexpr std::array types{"data"_L1, "index"_L1, "keys"_L1, "locks"_L1, "snapshots"_L1};

[[nodiscard]] bool isType(const QString &type) { return std::ranges::find(types, type) != types.end(); }

// A repository's name, as a destination's is (lower-case letters, digits and dashes).
[[nodiscard]] bool isRepository(const QString &name) {
    static const QRegularExpression allowed(u"^[a-z0-9][a-z0-9-]{0,63}$"_s);
    return allowed.match(name).hasMatch();
}

// One of restic's files, as its REST protocol names it ("config", "<type>/<id>"), where it is under
// the repository: a data file in the folder of its id's first two digits, as on a disk.
[[nodiscard]] std::optional<QString> placeOf(const QString &named) {
    if (named == u"config"_s)
        return named;
    static const QRegularExpression file(u"^([a-z]+)/([0-9a-f]{64})$"_s);
    const auto match = file.match(named);
    if (!match.hasMatch() || !isType(match.captured(1)))
        return std::nullopt;
    if (match.captured(1) == u"data"_s)
        return u"data/"_s + match.captured(2).left(2) + u'/' + match.captured(2);
    return named;
}

[[nodiscard]] Failure refused(const QString &what) { return {Failure::Code::HubStoreRefused, {what.left(120)}}; }

[[nodiscard]] QJsonArray listed(const QString &folder, bool nested) {
    QJsonArray files;
    QDirIterator each(folder, QDir::Files, nested ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
    static const QRegularExpression id(u"^[0-9a-f]{64}$"_s);
    while (each.hasNext()) {
        const QFileInfo file(each.next());
        if (id.match(file.fileName()).hasMatch())
            files << QJsonObject{{"name", file.fileName()}, {"size", file.size()}};
    }
    return files;
}

struct Asked {
    QString root; // the repository's folder
    QString file; // the file asked for, under it, if one is
};

[[nodiscard]] std::expected<QJsonObject, Failure> put(const Asked &asked, const QJsonObject &request) {
    const QString file = asked.root + u'/' + asked.file;
    const QString partial = file + u".partial"_s;
    const QByteArray data = QByteArray::fromBase64(request.value("data"_L1).toString().toLatin1());
    const qint64 at = request.value("at"_L1).toInteger();
    if (data.size() > HubStore::piece || !QDir().mkpath(QFileInfo(file).absolutePath()))
        return std::unexpected(refused(asked.file));
    QFile written(partial);
    // In order, a piece after the other; its first piece begins it anew.
    if ((at != 0 && at != written.size()) || !written.open(at == 0 ? QIODevice::WriteOnly | QIODevice::Truncate : QIODevice::Append)
        || written.write(data) != data.size())
        return std::unexpected(Failure(Failure::Code::HubStoreUnwritable, {asked.file, written.errorString()}));
    const qint64 size = written.size();
    written.close();
    if (request.value("last"_L1).toBool()) {
        std::ignore = QFile::remove(file); // none there, or one cut short before: the rename says
        if (QFile done(partial); !done.rename(file))
            return std::unexpected(Failure(Failure::Code::HubStoreUnwritable, {asked.file, done.errorString()}));
    }
    return QJsonObject{{"size", size}};
}

[[nodiscard]] std::expected<QJsonObject, Failure> get(const Asked &asked, const QJsonObject &request) {
    QFile read(asked.root + u'/' + asked.file);
    const qint64 length = request.value("length"_L1).toInteger(HubStore::piece);
    if (!read.open(QIODevice::ReadOnly) || length < 0 || length > HubStore::piece || !read.seek(request.value("at"_L1).toInteger()))
        return std::unexpected(refused(asked.file));
    return QJsonObject{{"data", QString::fromLatin1(read.read(length).toBase64())}, {"size", read.size()}};
}

} // namespace

QString HubStore::in(const QString &state) { return QDir(state).filePath(u"stores"_s); }

std::expected<QJsonObject, Failure> HubStore::answer(const QJsonObject &asked, const QString &computer) const {
    const QString repository = asked.value("repository"_L1).toString();
    const QString op = asked.value("op"_L1).toString();
    if (!HubComputers::isName(computer) || !isRepository(repository))
        return std::unexpected(refused(repository));
    Asked where{.root = QDir(m_folder).filePath(computer + u'/' + repository), .file = {}};
    if (op == u"create"_s) {
        for (const QLatin1StringView type : types)
            if (!QDir().mkpath(where.root + u'/' + type))
                return std::unexpected(Failure(Failure::Code::FolderUnmade, {where.root + u'/' + type}));
        qCInfo(lcHub).noquote() << computer << "keeps its snapshots here, as" << repository + u"."_s;
        return QJsonObject();
    }
    if (op == u"list"_s) {
        const QString type = asked.value("type"_L1).toString();
        if (!isType(type))
            return std::unexpected(refused(type));
        return QJsonObject{{"files", listed(where.root + u'/' + type, type == u"data"_s)}};
    }
    const auto file = placeOf(asked.value("path"_L1).toString());
    if (!file) {
        qCWarning(lcHub).noquote() << computer << "asked its snapshots' store for" << asked.value("path"_L1).toString().left(120)
                                   << ": refused.";
        return std::unexpected(refused(asked.value("path"_L1).toString()));
    }
    where.file = *file;
    if (op == u"stat"_s) {
        const QFileInfo info(where.root + u'/' + where.file);
        return QJsonObject{{"exists", info.isFile()}, {"size", info.isFile() ? info.size() : 0}};
    }
    if (op == u"get"_s)
        return get(where, asked);
    if (op == u"put"_s)
        return put(where, asked);
    if (op == u"remove"_s)
        return QJsonObject{{"removed", QFile::remove(where.root + u'/' + where.file)}};
    return std::unexpected(refused(op));
}
