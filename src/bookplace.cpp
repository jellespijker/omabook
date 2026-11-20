#include "bookplace.h"

#include <utility>

QJsonObject BookPlace::toJson() const { return toJson({}); }

QJsonObject BookPlace::toJson(QJsonObject alongside) const {
    alongside.insert("section", section);
    alongside.insert("fraction", fraction);
    return alongside;
}

BookPlace BookPlace::fromJson(const QJsonObject &object) { return fromJson(object, {}); }

BookPlace BookPlace::fromJson(const QJsonObject &object, const BookPlace &whenMissing) {
    return {.section = object.contains("section") ? object.value("section").toInt() : whenMissing.section,
            .fraction = object.contains("fraction") ? object.value("fraction").toDouble() : whenMissing.fraction};
}

QJsonObject Passage::toJson() const {
    QJsonObject object{{"from", from.toJson()}, {"to", to.toJson()}};
    for (const auto &[key, words] : {std::pair{"quote", quote}, std::pair{"before", before}, std::pair{"after", after}})
        if (!words.isEmpty())
            object.insert(key, words);
    return object;
}

Passage Passage::fromJson(const QJsonObject &object) {
    const BookPlace from = BookPlace::fromJson(object.value("from").toObject());
    // A passage that says only where it starts is the one place.
    return {.from = from,
            .to = object.contains("to") ? BookPlace::fromJson(object.value("to").toObject()) : from,
            .quote = object.value("quote").toString(),
            .before = object.value("before").toString(),
            .after = object.value("after").toString()};
}
