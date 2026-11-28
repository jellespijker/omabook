#include "syncguard.h"

#include <QJsonArray>
#include <QSet>
#include <algorithm>
#include <compare>
#include <utility>

#include "bookidentity.h"
#include "failure.h"
#include "hlc.h"
#include "log.h"
#include "snapshots.h"

using namespace Qt::StringLiterals;

namespace {

using SyncGuard::Breach;
using SyncGuard::Rule;

// What names a book and is said of it, but not a field: what the guard compares field by field.
[[nodiscard]] bool aField(const QString &key) { return key != "gid"_L1 && key != "names"_L1 && key != "identifiers"_L1; }

[[nodiscard]] bool empty(const QJsonValue &value) {
    switch (value.type()) {
    case QJsonValue::String:
        return value.toString().trimmed().isEmpty();
    case QJsonValue::Array:
        return value.toArray().isEmpty();
    case QJsonValue::Double:
        return value.toDouble() == 0;
    case QJsonValue::Bool:
        return !value.toBool();
    default:
        return value.isNull() || value.isUndefined();
    }
}

// A value as a person reads it, kept short.
[[nodiscard]] QString shown(const QJsonValue &value) {
    QString text;
    if (value.isArray())
        for (const auto &item : value.toArray())
            text += (text.isEmpty() ? u""_s : u", "_s) + item.toVariant().toString();
    else if (value.isBool())
        text = value.toBool() ? u"yes"_s : u"no"_s;
    else
        text = value.toVariant().toString();
    return text.size() > 60 ? text.left(59) + u'…' : text;
}

[[nodiscard]] QString titleOf(const SyncPlan::State &state) { return state.value("title").toString(); }

// A field of `before` that had a value and has none `after`, each.
void emptied(const SyncPlan::Change &change, QList<Breach> &breaches) {
    for (auto it = change.before.begin(); it != change.before.end(); ++it)
        if (aField(it.key()) && it.key() != "series_index"_L1 && !empty(*it) && empty(change.after.value(it.key())))
            breaches << Breach{
                .title = titleOf(change.before), .rule = Rule::FieldEmptied, .detail = it.key() + u" (\""_s + shown(*it) + u"\")"_s};
    const QJsonObject after = change.after.value("identifiers").toObject();
    const QJsonObject before = change.before.value("identifiers").toObject();
    for (auto it = before.begin(); it != before.end(); ++it)
        if (!empty(*it) && empty(after.value(it.key())))
            breaches << Breach{.title = titleOf(change.before), .rule = Rule::IdentifierCleared, .detail = it.key() + u' ' + shown(*it)};
}

// What a book paired on less than its global id would take of the other: its
// title, authors or ISBN without a rename agreed to, or any identifier (#158, #164).
void merged(const SyncPlan::Change &change, bool consent, QList<Breach> &breaches) {
    if (!change.weak)
        return;
    for (const char *field : {"title", "authors", "isbn"})
        if (!consent && change.before.value(QLatin1StringView(field)) != change.after.value(QLatin1StringView(field)))
            breaches << Breach{.title = titleOf(change.before),
                               .rule = Rule::BooksMerged,
                               .detail = QLatin1StringView(field) + u" \""_s + shown(change.after.value(QLatin1StringView(field))) + u'"'};
    const QJsonObject before = change.before.value("identifiers").toObject();
    const QJsonObject after = change.after.value("identifiers").toObject();
    for (auto it = after.begin(); it != after.end(); ++it)
        if (before.value(it.key()) != *it)
            breaches << Breach{.title = titleOf(change.before), .rule = Rule::BooksMerged, .detail = it.key() + u' ' + shown(*it)};
}

// An identifier announced as another book's, as "<title>: <scheme> <value>".
struct Named {
    QString title, scheme, value;
};

[[nodiscard]] Named namedIn(const QString &said) {
    const qsizetype space = said.lastIndexOf(u' ');
    const QString rest = said.left(space);
    const qsizetype colon = rest.lastIndexOf(": "_L1);
    return {.title = rest.left(colon), .scheme = rest.mid(colon + 2), .value = said.mid(space + 1)};
}

// Whether an identifier is the book's own (#166): its ISBN, or what its global id is made of.
[[nodiscard]] bool own(const QString &gid, const SyncPlan::State &state, const Named &named) {
    const std::optional<QString> isbn = BookIdentity::isbn13(named.value);
    if (named.scheme.compare("isbn"_L1, Qt::CaseInsensitive) == 0 && isbn && isbn == BookIdentity::isbn13(state.value("isbn").toString()))
        return true;
    const QStringList keys = BookIdentity::strongKeys(QJsonObject{{named.scheme, named.value}}, {});
    return std::ranges::any_of(keys, [&gid](const QString &key) { return gid == "e:"_L1 + key; });
}

void ownRemoved(const SyncGuard::Given &given, QList<Breach> &breaches) {
    for (const QString &said : given.identifiersRemoved) {
        const Named named = namedIn(said);
        for (auto it = given.mine.cbegin(); it != given.mine.cend(); ++it)
            if (titleOf(*it) == named.title && own(it.key(), *it, named))
                breaches << Breach{
                    .title = named.title, .rule = Rule::IdentifierCleared, .detail = named.scheme + u' ' + named.value + u", its own"_s};
    }
}

[[nodiscard]] QSet<QString> namesIn(const SyncPlan::States &states) {
    QSet<QString> names;
    for (const SyncPlan::State &state : states)
        for (const auto &name : state.value("names").toArray())
            names.insert(name.toString());
    return names;
}

// A book removed though the other side holds it still, by its id or by what names it.
void lost(const SyncPlan &plan, const SyncPlan::States &other, QList<Breach> &breaches) {
    const QSet<QString> held = namesIn(other);
    for (auto it = plan.removed.cbegin(); it != plan.removed.cend(); ++it) {
        const QJsonArray names = it->value("names").toArray();
        if (other.contains(it.key())
            || std::ranges::any_of(names, [&held](const QJsonValue &name) { return held.contains(name.toString()); }))
            breaches << Breach{.title = titleOf(*it), .rule = Rule::BookLost, .detail = {}};
    }
}

[[nodiscard]] QString counted(qsizetype count) { return count == 1 ? u"1 book"_s : QString::number(count) + u" books"_s; }

// What of a book differs from what was announced of it: each field, and each identifier but the ones announced gone.
void differing(const SyncPlan::State &expected, const SyncPlan::State &now, const QStringList &identifiersRemoved, QStringList &said) {
    const QString title = titleOf(expected);
    for (auto it = expected.begin(); it != expected.end(); ++it)
        if (aField(it.key()) && *it != now.value(it.key()))
            said << QString(R"(%1: its %2 is "%3", where the sync announced "%4")")
                        .arg(title, it.key(), shown(now.value(it.key())), shown(*it));
    const QJsonObject was = expected.value("identifiers").toObject();
    const QJsonObject is = now.value("identifiers").toObject();
    for (auto it = was.begin(); it != was.end(); ++it)
        if (is.value(it.key()) != *it && !identifiersRemoved.contains(title + u": "_s + it.key() + u' ' + shown(*it)))
            said << QString("%1: its identifier %2 %3 is gone, which the sync didn't announce").arg(title, it.key(), shown(*it));
    for (auto it = is.begin(); it != is.end(); ++it)
        if (!was.contains(it.key()))
            said << QString("%1: it gained the identifier %2 %3, which the sync didn't announce").arg(title, it.key(), shown(*it));
}

// The breaches of a plan, each with where it is, of the books not accepted.
struct Found {
    QStringList said;
    QStringList refusing;
};

void add(Found &found, const QList<Breach> &breaches, const QString &where, const QStringList &accepted) {
    for (const Breach &breach : breaches) {
        found.said << SyncGuard::worded(breach, where);
        if (!std::ranges::any_of(accepted,
                                 [&](const QString &title) { return title.trimmed().compare(breach.title, Qt::CaseInsensitive) == 0; }))
            found.refusing << found.said.last();
    }
}

[[nodiscard]] QString refusal(const QString &host, const QStringList &refusing) {
    return QString("The sync with %1 was refused before anything was written: it would break what a sync must never do. Nothing "
                   "was changed, here or on %1.\n  %2\nTo let a book's through as planned: omabook sync %1 --accept \"<title>\"; "
                   "add --dry-run to see the plan first.")
        .arg(host, refusing.join(u"\n  "_s));
}

// The breaches of both sides' plans, each with where it is (`where`, of there), of each side's books
// before the sync (`there` is none when the Omabook there can't say them).
[[nodiscard]] Found foundIn(const SyncSummary &planned, const SyncPlan::States &here, const std::optional<SyncPlan::States> &there,
                            const SyncOptions &options, const QString &where) {
    const bool consent = options.acceptRenames || options.details != SyncOptions::Details::Undecided;
    const QStringList removed = planned.titles.identifiersRemoved;
    Found found;
    if (planned.plans.here.said)
        add(found,
            SyncGuard::check(
                planned.plans.here,
                {.mine = here, .other = there.value_or(SyncPlan::States()), .consent = consent, .identifiersRemoved = removed}),
            u"here"_s, options.accept);
    if (planned.plans.there.said && there)
        add(found,
            SyncGuard::check(planned.plans.there, {.mine = *there, .other = here, .consent = consent, .identifiersRemoved = removed}),
            where, options.accept);
    return found;
}

// Whether the other side made no plan, an Omabook there from before the guard: its side goes unchecked,
// and is said to; a hub's is refused (#211).
[[nodiscard]] bool unchecked(const SyncPlan &there, const SyncGuard::Sides &sides) {
    if (!there.said)
        qCWarning(lcSync).noquote() << sides.host << "made no plan for the sync guard, being an older Omabook:"
                                    << (sides.hub ? "a hub, refused" : "its side goes unchecked");
    return !there.said;
}

// A sync refused before anything was written (a dry run's, with `dryRun`): what its plan found, and why.
[[nodiscard]] SyncSummary refused(SyncSummary planned, const QString &why, bool dryRun) {
    SyncSummary summary;
    summary.dryRun = dryRun;
    summary.breaches = std::move(planned.breaches);
    summary.plans = std::move(planned.plans);
    summary.uncheckedThere = planned.uncheckedThere;
    summary.error = why;
    return summary;
}

} // namespace

QList<Breach> SyncGuard::check(const SyncPlan &plan, const Given &given) {
    QList<Breach> breaches;
    for (const SyncPlan::Change &change : plan.books) {
        emptied(change, breaches);
        merged(change, given.consent, breaches);
    }
    ownRemoved(given, breaches);
    lost(plan, given.other, breaches);
    for (const SyncPlan::Place &place : plan.places)
        if (std::is_lt(Hlc::compare(place.after, place.before)))
            breaches << Breach{.title = place.title, .rule = Rule::PlaceBack, .detail = {}};
    for (const QString &title : plan.annotationsMoved)
        breaches << Breach{.title = title, .rule = Rule::AnnotationMoved, .detail = {}};
    return breaches;
}

QStringList SyncGuard::differences(const SyncPlan::States &before, const SyncPlan::States &after, const Announced &announced) {
    QStringList said;
    for (auto it = before.cbegin(); it != before.cend(); ++it) {
        const auto now = after.constFind(it.key());
        if (now == after.cend()) {
            if (!announced.plan.removed.contains(it.key()))
                said << QString("%1 is gone, which the sync didn't announce").arg(titleOf(*it));
            continue;
        }
        const auto change = announced.plan.books.constFind(it.key());
        differing(change == announced.plan.books.cend() ? *it : change->after, *now, announced.identifiersRemoved, said);
    }
    QStringList arrived;
    for (auto it = after.cbegin(); it != after.cend(); ++it)
        if (!before.contains(it.key()))
            arrived << titleOf(*it);
    if (arrived.size() > announced.received)
        said << QString("%1 more than the sync announced arrived: %2")
                    .arg(counted(arrived.size() - announced.received), arrived.join(", "_L1));
    return said;
}

QString SyncGuard::worded(const Breach &breach, const QString &where) {
    QString what;
    switch (breach.rule) {
    case Rule::BookLost:
        what = u"would be removed, though the other side still has it"_s;
        break;
    case Rule::IdentifierCleared:
        what = u"would lose its identifier "_s + breach.detail;
        break;
    case Rule::FieldEmptied:
        what = u"would lose its "_s + breach.detail;
        break;
    case Rule::BooksMerged:
        what = u"would take the "_s + breach.detail
               + u" of a book it is paired with by an identifier, not by its id, with no rename agreed to"_s;
        break;
    case Rule::PlaceBack:
        what = u"its reading place would go back to one read earlier"_s;
        break;
    case Rule::AnnotationMoved:
        what = u"a bookmark or highlight would move to it from another book"_s;
        break;
    }
    return QString("%1 (%2): %3").arg(breach.title, where, what);
}

namespace {

// What came of the moment snapshot taken before a sync wrote (#291).
void noteMoment(SyncSummary &summary, const std::expected<std::optional<Snapshots::Taken>, Failure> &moment) {
    if (!moment) {
        qCWarning(lcSync).noquote() << "no moment snapshot before a sync:" << moment.error().english();
        summary.noSnapshot = moment.error().text();
    } else if (*moment && !(*moment)->unchanged) {
        summary.momentSnapshot = (*moment)->id.left(8);
    }
}

} // namespace

SyncSummary SyncGuard::guarded(const Library &library, const Sides &sides, const SyncOptions &options) {
    const SyncPlan::States hereBefore = SyncPlan::states(library);
    const std::optional<SyncPlan::States> thereBefore = sides.theirStates();
    SyncOptions dry = options;
    dry.dryRun = true;
    SyncSummary planned = sides.run(dry);
    if (!planned.error.isEmpty())
        return planned;
    const QStringList removed = planned.titles.identifiersRemoved;
    const Found found = foundIn(planned, hereBefore, thereBefore, options, u"on "_s + sides.host);
    planned.breaches = found.said;
    planned.uncheckedThere = unchecked(planned.plans.there, sides);
    if (planned.uncheckedThere && sides.hub)
        return refused(std::move(planned), Failure(Failure::Code::HubSyncHubNoPlan, {sides.host}).text(), options.dryRun);
    if (options.dryRun)
        return planned;
    if (!found.refusing.isEmpty()) {
        qCWarning(lcSync).noquote() << "a sync with" << sides.host << "refused:" << found.refusing.size() << "breaches of the guard";
        return refused(std::move(planned), refusal(sides.host, found.refusing), false);
    }
    const auto moment = Snapshots(SnapshotPlace::here()).before(library, SnapshotReason::Sync);
    SyncSummary done = sides.run(options);
    noteMoment(done, moment);
    done.breaches = found.said;
    done.uncheckedThere = planned.uncheckedThere;
    if (!done.error.isEmpty())
        return done;
    for (const QString &line : differences(hereBefore, SyncPlan::states(library),
                                           {.plan = planned.plans.here, .received = planned.received, .identifiersRemoved = removed}))
        done.unannounced << u"Here: "_s + line;
    const std::optional<SyncPlan::States> thereAfter = thereBefore && planned.plans.there.said ? sides.theirStates() : std::nullopt;
    if (thereAfter)
        for (const QString &line :
             differences(*thereBefore, *thereAfter, {.plan = planned.plans.there, .received = planned.sent, .identifiersRemoved = removed}))
            done.unannounced << u"On "_s + sides.host + u": "_s + line;
    if (!done.unannounced.isEmpty())
        qCWarning(lcSync).noquote() << "a sync with" << sides.host << "did" << done.unannounced.size() << "things it didn't announce";
    done.plans = std::move(planned.plans);
    return done;
}
