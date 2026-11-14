#pragma once

#include <QString>
#include <QVariantMap>

#include "customization.h"
#include "prompts.h"
#include "settings.h"

// Changing the interface files on someone's behalf (the "change this"
// prompt, `omabook change`), carefully: what was there is kept so the change
// can be undone, and a change the files' own checks refuse is undone at once.
namespace InterfaceChanges {

// The files and settings as they are, for the model to start from.
Prompts::InterfaceState state(const Customization &custom, const Settings &settings, const QVariantMap &colors);

// Makes the change. Empty when it took; otherwise what was wrong with it,
// with everything put back as it was.
QString apply(Customization &custom, Settings &settings, const Prompts::InterfaceChange &change);

bool canUndo(const Customization &custom);
// Puts back what the last change replaced. False when there is nothing to undo.
bool undo(Customization &custom, Settings &settings);

} // namespace InterfaceChanges
