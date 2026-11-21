#include "heldspaces.h"

#include <QSqlQuery>
#include <QVariant>
#include <utility>

#include "changelog.h"
#include "library.h"
#include "person.h"
#include "sharedspace.h"

using namespace Qt::StringLiterals;

QString HeldSpaces::memberName(const Library &library, const QString &space, const QString &person) {
    QSqlQuery query = ChangeLog::query(library, "SELECT name FROM space_members WHERE space = ? AND person = ?", {space, person});
    return query.next() ? query.value(0).toString() : QString();
}

QList<HeldSpaces::Held> HeldSpaces::held(const Library &library) {
    QList<Held> spaces;
    for (const SubscribedSpace::Subscription &space : SubscribedSpace::list(library))
        spaces << Held{.space = space,
                       .kind = u"published"_s,
                       .role = QString::fromLatin1(SubscribedSpace::readerRole),
                       .me = {},
                       .parts = {},
                       .ops = SubscribedSpace::operations(library, space.space)};
    for (const QString &space : SharedSpace::spaces(library)) {
        SharedSpace::State state = SharedSpace::state(library, space);
        const QString me = SharedSpace::memberAs(library, state);
        spaces << Held{.space = {.space = space, .name = state.name, .owner = state.founder, .ownerName = {}, .collection = 0},
                       .kind = u"shared"_s,
                       .role = SharedSpace::roleName(state.roleOf(me)),
                       .me = me.isEmpty() && library.person() != nullptr ? library.person()->id() : me,
                       .parts = state.parts,
                       .ops = std::move(state.accepted)};
    }
    return spaces;
}
