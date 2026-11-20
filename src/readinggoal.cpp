#include "readinggoal.h"

#include <QJsonDocument>
#include <QJsonObject>

#include "changelog.h"
#include "changelogfields.h"
#include "failure.h"
#include "library.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

// The change log's thing a year's goal is, and its one field: the kind and
// the target together, so the two always change as one.
constexpr auto entity = "goal"_L1;
constexpr auto field = "goal"_L1;

[[nodiscard]] QString idOf(int year) { return QString::number(year); }

} // namespace

QString ReadingGoal::nameOf(Kind kind) {
    switch (kind) {
    case Kind::Books:
        return u"books"_s;
    case Kind::Hours:
        return u"hours"_s;
    case Kind::None:
        break;
    }
    return {};
}

std::expected<ReadingGoal::Kind, QString> ReadingGoal::kindNamed(const QString &name) {
    const QString word = name.trimmed().toLower();
    if (word == "books"_L1 || word == "book"_L1)
        return Kind::Books;
    if (word == "hours"_L1 || word == "hour"_L1)
        return Kind::Hours;
    if (word.isEmpty() || word == "none"_L1)
        return Kind::None;
    return std::unexpected(Failure(Failure::Code::GoalUnknownKind, {name}).text());
}

ReadingGoal::Goal ReadingGoal::of(const Library &library, int year) {
    const ChangeLogFields::Map fields = ChangeLogFields::logged(library, QString(entity), idOf(year));
    const auto latest = fields.constFind(QString(field));
    if (latest == fields.cend())
        return {.year = year};
    const QJsonObject value = QJsonDocument::fromJson(latest->value.toUtf8()).object();
    const auto kind = kindNamed(value.value("kind"_L1).toString());
    const int target = value.value("target"_L1).toInt();
    if (!kind || target <= 0)
        return {.year = year};
    return {.year = year, .kind = *kind, .target = target};
}

std::expected<void, QString> ReadingGoal::set(const Library &library, const Goal &goal) {
    if (goal.year < 1 || goal.year > 9999)
        return std::unexpected(Failure(Failure::Code::NoSuchYear, {QString::number(goal.year)}).text());
    if (goal.target < 0 || goal.target > 100000)
        return std::unexpected(Failure(Failure::Code::GoalOutOfRange).text());
    const QJsonObject value = goal.set() ? QJsonObject{{"kind"_L1, nameOf(goal.kind)}, {"target"_L1, goal.target}} : QJsonObject{};
    const QString said = QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact));
    const auto same = [&goal](const Goal &now) {
        return now.set() == goal.set() && (!goal.set() || (now.kind == goal.kind && now.target == goal.target));
    };
    if (same(of(library, goal.year)))
        return {};
    {
        // At once: the log is read for the next number before the operation
        // is written, and a library another connection (the statistics
        // counted beside the window) writes meanwhile would refuse the write
        // of a transaction begun only as a read.
        const Library::Batch batch(library, Library::Batch::Lock::AtOnce);
        if (!batch.begun())
            return std::unexpected(Failure(Failure::Code::LibraryBusy).text());
        ChangeLog::append(library, {Spaces::personalSpace(library), QString(entity), idOf(goal.year), QString(field), said, std::nullopt});
    }
    if (!same(of(library, goal.year)))
        return std::unexpected(Failure(Failure::Code::GoalNotKept).text());
    return {};
}
