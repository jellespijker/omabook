#include "annotationcontext.h"

#include <QObject>
#include <algorithm>
#include <ranges>

#include "ai.h"
#include "aiendpoint.h"
#include "highlightcolors.h"
#include "lineink.h"
#include "recognitionroute.h"
#include "settings.h"

using namespace Qt::StringLiterals;

namespace {

// The most annotations sent with one question: a reader's notes on a long
// book run to hundreds, and those nearest where they are matter most.
constexpr qsizetype mostLines = 200;

[[nodiscard]] QString whereOf(const BookPlace &place) { return u"section %1, %2%"_s.arg(place.section).arg(qRound(place.fraction * 100)); }

[[nodiscard]] QString lineOf(const Annotation &annotation, const QVariantMap &colorNames) {
    const QString quote = annotation.passage().quote.simplified();
    if (annotation.kind == "bookmark")
        return u"bookmark (%1): %2"_s.arg(whereOf(annotation.place), annotation.text.simplified());
    if (annotation.kind == "margin-note")
        return u"margin note (%1) on \"%2\": %3"_s.arg(whereOf(annotation.place), quote, annotation.note.simplified());
    if (LineInk::isKind(annotation.kind))
        return u"handwriting (%1) beside \"%2\""_s.arg(whereOf(annotation.place), quote);
    const QString key = HighlightColors::of(annotation.color).key;
    const QString named = colorNames.value(key).toString().trimmed();
    QString line =
        u"highlight in %1 (%2): \"%3\""_s.arg(named.isEmpty() ? key : key + " \"" + named + "\"", whereOf(annotation.place), quote);
    if (!annotation.note.trimmed().isEmpty())
        line += " with the note: " + annotation.note.simplified();
    return line;
}

} // namespace

QString AnnotationContext::whyNot(const Settings &settings) {
    const AiChoice choice = Ai::choose(settings);
    if (!choice.available())
        return choice.problem;
    if (choice.id == "openai"_L1) {
        // Omarchy's local models run on this computer or the tailnet.
        if (AiEndpoints::preset(settings) == "omarchy-local"_L1)
            return {};
        const QString model = Ai::models(settings, choice.modelsKey()).main;
        if (RecognitionRoute::whyNot(settings, AiEndpoints::baseUrl(settings), model, true).isEmpty())
            return {};
    } else if (settings.recognitionHosted()) {
        return {};
    }
    // The question asked is the asking for it; what is left is the setting.
    return QObject::tr("Your highlights and notes weren't sent to %1: it is someone else's computer, and they go only to your own "
                       "(this computer, your tailnet) unless you allow hosted servers for recognition in the settings.")
        .arg(choice.name);
}

bool AnnotationContext::reached(const Annotation &annotation, const BookPlace &upTo) {
    return std::max(annotation.place, annotation.to) <= upTo;
}

QString AnnotationContext::lines(const QList<Annotation> &annotations, const BookPlace &upTo, const QVariantMap &colorNames) {
    const auto told = [&upTo](const Annotation &annotation) {
        const bool worded = annotation.kind == "bookmark" ? !annotation.text.trimmed().isEmpty() : annotation.hasPassage();
        return worded && !annotation.lost && reached(annotation, upTo);
    };
    QList<Annotation> chosen = annotations | std::views::filter(told) | std::ranges::to<QList<Annotation>>();
    std::ranges::stable_sort(chosen, {}, [](const Annotation &annotation) { return annotation.place; });
    if (chosen.size() > mostLines)
        chosen = chosen.last(mostLines);
    QStringList said;
    for (const Annotation &annotation : std::as_const(chosen))
        said << lineOf(annotation, colorNames);
    return said.join('\n');
}

BookPlace AnnotationContext::cutFor(const Library &library, qint64 bookId, int upToSection) {
    const BookPlace reader = library.progress(bookId).place;
    if (upToSection < 0)
        return reader;
    return std::min(reader, BookPlace{.section = upToSection, .fraction = 1});
}

QString AnnotationContext::forQuestion(const Library &library, qint64 bookId, const BookPlace &upTo, const Settings &settings) {
    QString marks = lines(library.annotations(bookId), upTo, settings.highlightNames());
    if (marks.isEmpty() || !whyNot(settings).isEmpty())
        return {};
    return marks;
}
