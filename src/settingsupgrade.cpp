// The shapes config.json has had (Settings, settings.h), and how an older file
// is carried forward to the one this Omabook writes.
#include "settings.h"

#include "pagelayout.h"

namespace {

// The shape of config.json. It goes up when a key is renamed or restructured;
// the step that carries an older file forward is added to `upgraded` below,
// so a setting made in an older Omabook is never silently dropped. Keys that
// are only added need no step: a missing key reads as its default.
constexpr int configVersion = 3;

// The count of pages the older "spread" asked for.
[[nodiscard]] QJsonValue pagesOf(const QString &spread) {
    if (spread == "always")
        return 2;
    if (spread == "never")
        return 1;
    return "auto";
}

} // namespace

QJsonObject Settings::upgraded(QJsonObject values) {
    const int from = values.value("config_version").toInt(0);
    // 2: "justify" (true or false) became "align", which has four choices.
    if (from < 2 && values.contains("justify")) {
        if (!values.contains("align"))
            values.insert("align", values.value("justify").toBool() ? "justify" : "start");
        values.remove("justify");
    }
    // 3: the longest line became a measure in characters ("measure"), from
    // pixels ("line_width"), and the choice of two pages ("spread": "auto",
    // "always" or "never") a count of pages ("pages": "auto", 1, 2 or 3).
    // A width left at the old default is the new default.
    if (from < 3) {
        if (values.contains("line_width")) {
            const int pixels = values.value("line_width").toInt(720);
            if (!values.contains("measure") && pixels != 720)
                values.insert("measure", PageLayout::measureFromPixels(pixels, values.value("font_size").toInt(19)));
            values.remove("line_width");
        }
        if (values.contains("spread")) {
            if (!values.contains("pages"))
                values.insert("pages", pagesOf(values.value("spread").toString()));
            values.remove("spread");
        }
    }
    // A file from a newer Omabook is read as it is: keys this one doesn't
    // know are kept and written back untouched.
    if (from < configVersion)
        values.insert("config_version", configVersion);
    return values;
}
