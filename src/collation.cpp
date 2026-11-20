#include "collation.h"

namespace Collation {

QLocale localeFor(const QLocale &system) {
    // CLDR's English has no collation tailoring of its own: its order is the
    // root (Unicode default) collation, the one meant when no language is.
    return system.language() == QLocale::C ? QLocale(QLocale::English) : system;
}

QCollator natural(const QLocale &system) {
    QCollator collator(localeFor(system));
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    return collator;
}

} // namespace Collation
