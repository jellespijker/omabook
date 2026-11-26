#include "graphbuilt.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <array>
#include <utility>

#include "changelog.h"
#include "changelogfields.h"
#include "library.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

constexpr std::array states{std::pair{GraphBuilt::State::Building, "building"_L1}, std::pair{GraphBuilt::State::Built, "built"_L1},
                            std::pair{GraphBuilt::State::Stopped, "stopped"_L1}};

// The book's field it is kept in.
constexpr auto field = "graph"_L1;

} // namespace

QString GraphBuilt::Mark::written() const {
    QJsonObject object{{"by", by}, {"hub", hub}, {"at", at.toSecsSinceEpoch()}};
    for (const auto &[one, name] : states)
        if (one == state)
            object.insert("state"_L1, name);
    if (to)
        object.insert("to"_L1, to->toJson());
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

std::optional<GraphBuilt::Mark> GraphBuilt::Mark::read(const QString &value) {
    const QJsonObject object = QJsonDocument::fromJson(value.toUtf8()).object();
    const QString said = object.value("state"_L1).toString();
    for (const auto &[state, name] : states)
        if (said == name) {
            Mark mark{.state = state,
                      .by = object.value("by"_L1).toString(),
                      .hub = object.value("hub"_L1).toBool(),
                      .at = QDateTime::fromSecsSinceEpoch(object.value("at"_L1).toInteger()),
                      .to = {}};
            if (object.value("to"_L1).isObject())
                mark.to = BookPlace::fromJson(object.value("to"_L1).toObject());
            return mark;
        }
    return std::nullopt;
}

std::optional<GraphBuilt::Mark> GraphBuilt::of(const Library &library, qint64 book) {
    const QString gid = Spaces::bookId(library, book);
    if (gid.isEmpty())
        return std::nullopt;
    const ChangeLogFields::Map logged = ChangeLogFields::logged(library, u"book"_s, gid);
    const auto said = logged.constFind(QString(field));
    return said == logged.cend() ? std::nullopt : Mark::read(said->value);
}

void GraphBuilt::mark(const Library &library, qint64 book, const Mark &mark) {
    const QString gid = Spaces::bookId(library, book);
    if (gid.isEmpty())
        return;
    const Library::Batch batch(library);
    ChangeLog::append(library, {.space = Spaces::personalSpace(library),
                                .entity = u"book"_s,
                                .id = gid,
                                .field = QString(field),
                                .value = mark.written(),
                                .hlc = std::nullopt});
}

bool GraphBuilt::leftToHub(const std::optional<Mark> &mark, const QDateTime &now) {
    if (!mark || !mark->hub)
        return false;
    if (mark->state == State::Built)
        return true;
    return mark->state == State::Building && mark->at.secsTo(now) <= std::chrono::seconds(patience).count();
}
