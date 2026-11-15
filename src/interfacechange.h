#pragma once

#include <QString>
#include <QVariantMap>

#include "customization.h"
#include "library.h"
#include "prompts.h"
#include "settings.h"

// Changing the interface files on someone's behalf (the "change this"
// prompt, `omabook change`), carefully: what was there is kept so the change
// can be undone, and a change the files' own checks refuse is undone at once.
namespace InterfaceChanges {

// The files and settings as they are, for the model to start from; with a
// library, its collections and the tags its books have, so a change can make
// a collection ("a collection of my programming books").
Prompts::InterfaceState state(const Customization &custom, const Settings &settings, const QVariantMap &colors,
                              const Library *library = nullptr);

// Makes the change, the library's part of it (its collections) only when
// given one. Empty when it took; otherwise what was wrong with it, with
// everything put back as it was.
QString apply(Customization &custom, Settings &settings, const Prompts::InterfaceChange &change, Library *library = nullptr);

bool canUndo(const Customization &custom);
// Puts back what the last change replaced: collections it made go again,
// and those it gave another rule get their own back. False when there is
// nothing to undo.
bool undo(Customization &custom, Settings &settings, Library *library = nullptr);

} // namespace InterfaceChanges
