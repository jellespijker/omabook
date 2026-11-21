#include "heldspaces.h"

#include <optional>
#include <utility>

#include "library.h"
#include "membernames.h"
#include "person.h"
#include "sharedspace.h"
#include "untrustedtext.h"

using namespace Qt::StringLiterals;

QList<HeldSpaces::Held> HeldSpaces::held(const Library &library) {
    QList<Held> spaces;
    for (const SubscribedSpace::Subscription &space : SubscribedSpace::list(library)) {
        QHash<QString, QString> names;
        if (const std::optional<QString> owner = UntrustedText::name(space.ownerName))
            names.insert(space.owner, *owner);
        spaces << Held{.space = space,
                       .kind = u"published"_s,
                       .role = QString::fromLatin1(SubscribedSpace::readerRole),
                       .me = {},
                       .parts = {},
                       .ops = SubscribedSpace::operations(library, space.space),
                       .names = std::move(names)};
    }
    for (const QString &space : SharedSpace::spaces(library)) {
        SharedSpace::State state = SharedSpace::state(library, space);
        const QString me = SharedSpace::memberAs(library, state);
        QHash<QString, QString> names = MemberNames::of(state.accepted);
        spaces << Held{.space = {.space = space, .name = state.name, .owner = state.founder, .ownerName = {}, .collection = 0},
                       .kind = u"shared"_s,
                       .role = SharedSpace::roleName(state.roleOf(me)),
                       .me = me.isEmpty() && library.person() != nullptr ? library.person()->id() : me,
                       .parts = state.parts,
                       .ops = std::move(state.accepted),
                       .names = std::move(names)};
    }
    return spaces;
}
