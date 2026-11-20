#include "textmarks.h"

#include "graphlayers.h"
#include "hyphenation.h"

#include <QRegularExpression>
#include <QTextCursor>
#include <QTextDocument>
#include <algorithm>
#include <cmath>
#include <ranges>

namespace TextMarks {

namespace {

// The kinds whose names are proper names, written with a capital wherever
// the text means them. The others (an idea, a thing, an event) may be
// labelled with capitals in the graph and written without them in the book.
[[nodiscard]] bool proper(const QString &kind) { return kind == "character" || kind == "place" || kind == "group"; }

[[nodiscard]] bool article(const QString &folded) { return folded == "the" || folded == "a" || folded == "an"; }

// Words that come before a name and are no name of their own.
[[nodiscard]] bool title(const QString &folded) {
    static const QSet<QString> titles{"mr",      "mrs",     "ms",     "miss",   "dr",       "sir",     "dame",   "lady",
                                      "lord",    "king",    "queen",  "prince", "princess", "duke",    "duchess", "count",
                                      "countess", "baron",  "captain", "brother", "sister",  "father",  "mother", "uncle",
                                      "aunt",    "saint",   "st",     "master", "mistress", "general", "doctor", "professor",
                                      "old",     "young",   "little", "big",    "of",       "the",     "van",    "von",
                                      "de",      "du",      "da",     "del",    "la",       "le"};
    return titles.contains(folded);
}

[[nodiscard]] bool wordChar(QChar c) {
    return c.isLetterOrNumber() || c.isMark() || c.isSurrogate();
}

// What may stand between two words of one name: a space (not the end of a
// line or paragraph), a hyphen or an apostrophe, and not much of it.
[[nodiscard]] bool joins(QStringView gap) {
    if (gap.isEmpty() || gap.size() > 3)
        return false;
    return std::ranges::all_of(gap, [](QChar c) {
        if (c == '\n' || c == '\r' || c == QChar::ParagraphSeparator || c == QChar::LineSeparator)
            return false;
        return c.isSpace() || c == '-' || c == QChar(0x2010) || c == QChar(0x2011) || c == '\'' || c == QChar(0x2019);
    });
}

// Whether the word starting at `at` begins a sentence: nothing but quotes,
// brackets and dashes between it and the end of the last sentence, the
// start of its paragraph, or the start of the text.
[[nodiscard]] bool startsSentence(QStringView text, qsizetype at) {
    static const QString skipped = QStringLiteral("\"'“”‘’«»„‹›([{—–-*_ ");
    for (qsizetype i = at - 1; i >= 0; --i) {
        const QChar c = text[i];
        if (c == '\n' || c == '\r' || c == QChar::ParagraphSeparator || c == QChar::LineSeparator || c == QChar::ObjectReplacementCharacter)
            return true;
        if (c.isSpace() || skipped.contains(c))
            continue;
        return c == '.' || c == '!' || c == '?' || c == QChar(0x2026);
    }
    return true;
}

[[nodiscard]] bool capitalised(QStringView word) { return !word.isEmpty() && word.front().isUpper(); }

[[nodiscard]] QStringList wordsOf(const QString &name) {
    QStringList found;
    for (const Word &word : words(name))
        found << name.mid(word.start, word.length);
    return found;
}

[[nodiscard]] QStringList folded(const QStringList &words) {
    QStringList out;
    out.reserve(words.size());
    for (const QString &word : words)
        out << word.toCaseFolded();
    return out;
}

} // namespace

bool marked(const QString &kind) {
    static const QSet<QString> kinds{"character", "place", "group", "concept", "item", "event", "topic"};
    return kinds.contains(kind);
}

qsizetype reach(const QList<std::pair<GraphNode, GraphPlace>> &met, const GraphPlace &place) {
    // The list is in the order things were met, so what is known at a place
    // is its front: the first thing met after the place ends it.
    const auto after = std::ranges::find_if(met, [&place](const auto &entry) { return !entry.second.notAfter(place); });
    return after - met.begin();
}

QList<Thing> known(const QList<std::pair<GraphNode, GraphPlace>> &met, const GraphPlace &place) {
    QList<Thing> things;
    for (const auto &[node, at] : met | std::views::take(reach(met, place)))
        if (marked(node.kind))
            things.append({node.id, node.kind, node.name, node.summary, node.aliases(), at, GraphLayers::from(node)});
    return things;
}

QList<Word> words(QStringView text) {
    QList<Word> found;
    qsizetype start = -1;
    for (qsizetype i = 0; i <= text.size(); ++i) {
        const bool inside = i < text.size() && wordChar(text[i]);
        if (inside && start < 0)
            start = i;
        else if (!inside && start >= 0) {
            found.append({start, i - start});
            start = -1;
        }
    }
    return found;
}

bool everyday(const QString &lower) {
    // English words that are also names, and the words a name is most
    // easily mistaken for. The chapter's own lower-case words catch the
    // rest, in any language (see Matcher::match).
    static const QSet<QString> words{
        "will",   "hope",   "grace",  "faith",  "joy",     "mark",    "rose",    "may",     "june",    "april",
        "august", "bill",   "art",    "rob",    "sue",     "pat",     "jack",    "frank",   "earl",    "dawn",
        "summer", "autumn", "winter", "spring", "sky",     "river",   "stone",   "wood",    "hunter",  "page",
        "cook",   "baker",  "chance", "patience", "trust", "honor",   "honour",  "hero",    "king",    "queen",
        "prince", "captain", "doctor", "father", "mother", "sister",  "brother", "lord",    "lady",    "master",
        "guard",  "watch",  "rain",   "storm",  "snow",    "star",    "sun",     "moon",    "night",   "day",
        "light",  "dark",   "fall",   "lane",   "hall",    "glen",    "dale",    "hill",    "brook",   "ford",
        "carter", "fisher", "mason",  "taylor", "young",   "little",  "white",   "black",   "brown",   "green",
        "gray",   "grey",   "red",    "gold",   "silver",  "iron",    "glass",   "salt",    "ash",     "rook",
        "wren",   "crow",   "fox",    "wolf",   "bear",    "hawk",    "raven",   "lark",    "robin",   "drew",
        "max",    "mercy",  "temperance", "prudence", "constance", "ruby", "pearl", "amber", "ivy",  "holly",
        "heather", "violet", "lily",  "daisy",  "iris",    "sage",    "reed",    "rush",    "miles",   "chase",
        "gene",   "ray",    "guy",    "sonny",  "nick",    "don",     "dean",    "major",   "chief",   "sergeant",
        "tide",   "harbour", "harbor", "city",  "court",   "guild",   "order",   "church",  "house",   "tower",
        "one",    "nobody", "someone", "father", "god",    "fate",    "death",   "love",    "war",     "peace"};
    return words.contains(lower);
}

Matcher::Matcher(const QList<Thing> &things) {
    // How many things go by each word of their names, for the parts of a
    // character's name: a part two things share names neither.
    QHash<QString, QSet<qint64>> users;
    QSet<QString> wholeOneWord;
    for (const Thing &thing : things)
        for (const QString &name : QStringList{thing.name} + thing.aliases) {
            const QStringList folds = folded(wordsOf(name));
            for (const QString &word : folds)
                users[word].insert(thing.id);
            if (folds.size() == 1)
                wholeOneWord.insert(folds.first());
        }

    struct Made {
        Term term;
        int order = 0;
    };
    QList<Made> made;
    QSet<QStringList> taken;
    int order = 0;
    // Whether the capitals of a name count: always for a proper name, and
    // for another when it is written in title case.
    const auto strictFor = [](const QStringList &words, const Thing &thing) {
        if (proper(thing.kind))
            return true;
        const qsizetype skip = !words.isEmpty() && article(words.first().toCaseFolded()) ? 1 : 0;
        return words.size() > 1
               && std::ranges::all_of(words | std::views::drop(skip),
                                      [](const QString &word) { return capitalised(word) || !word.front().isLetter(); });
    };
    const auto add = [&](const QStringList &words, const Thing &thing, bool part, bool strict) {
        if (words.isEmpty())
            return;
        Term term{words, folded(words), thing.id, part, strict};
        // One letter is no name to find.
        if (term.folded.size() == 1 && term.folded.first().size() < 2)
            return;
        made.append({term, order++});
    };
    for (const Thing &thing : things) {
        for (const QString &name : QStringList{thing.name} + thing.aliases) {
            const QStringList written = wordsOf(name);
            const bool strict = strictFor(written, thing);
            add(written, thing, false, strict);
            // "The Tide Guild" is also "a Tide Guild cutter", and as strict.
            if (written.size() > 1 && article(written.first().toCaseFolded()))
                add(written.mid(1), thing, false, strict);
        }
        if (thing.kind != "character")
            continue;
        for (const QString &word : wordsOf(thing.name)) {
            const QString fold = word.toCaseFolded();
            if (word.size() >= 3 && capitalised(word) && !title(fold) && users.value(fold).size() == 1 && !wholeOneWord.contains(fold)
                && wordsOf(thing.name).size() > 1)
                add({word}, thing, true, true);
        }
    }
    // The longest first; at one length a whole name before a part of one,
    // then the thing met first.
    std::ranges::stable_sort(made, [](const Made &a, const Made &b) {
        if (a.term.folded.size() != b.term.folded.size())
            return a.term.folded.size() > b.term.folded.size();
        if (a.term.part != b.term.part)
            return !a.term.part;
        return a.order < b.order;
    });
    size_t print = 0;
    for (const Made &one : made) {
        if (taken.contains(one.term.folded))
            continue;
        taken.insert(one.term.folded);
        m_first[one.term.folded.first()].append(one.term);
        ++m_terms;
        print = qHashMulti(print, one.term.words, one.term.thing, one.term.part, one.term.strict);
    }
    // Whether a term's capitals count depends on the kind of its thing.
    for (const Thing &thing : things) {
        m_kinds.insert(thing.id, thing.kind);
        print = qHashMulti(print, thing.id, thing.kind);
    }
    m_fingerprint = print;
}

QList<Mark> Matcher::matchShown(QStringView text) const {
    if (!text.contains(Hyphenation::softHyphen))
        return match(text);
    // A name with a soft hyphen in it is still the name: it is looked for
    // without them, and marked where it is shown.
    const Hyphenation::Plain plain(text);
    QList<Mark> marks = match(plain.text());
    for (Mark &mark : marks) {
        const qsizetype end = plain.toShownEnd(mark.start + mark.length);
        mark.start = plain.toShown(mark.start);
        mark.length = end - mark.start;
    }
    return marks;
}

QList<Mark> Matcher::match(QStringView text) const {
    QList<Mark> marks;
    if (m_first.isEmpty())
        return marks;
    const QList<Word> found = words(text);
    QStringList folds;
    folds.reserve(found.size());
    // The words this chapter uses in lower case: a name spelled like one
    // of them is an everyday word here, in whatever language the book is.
    QSet<QString> lower;
    for (const Word &word : found) {
        const QStringView written = text.mid(word.start, word.length);
        folds << written.toString().toCaseFolded();
        if (written.front().isLower())
            lower.insert(folds.last());
    }
    const auto joined = [&](qsizetype one, qsizetype other) {
        const Word &a = found.at(std::min(one, other)), &b = found.at(std::max(one, other));
        return joins(text.mid(a.start + a.length, b.start - a.start - a.length));
    };
    const auto capitalAt = [&](qsizetype index) { return capitalised(text.mid(found.at(index).start, found.at(index).length)); };
    const auto inName = [&](qsizetype index) {
        return (index > 0 && joined(index - 1, index) && capitalAt(index - 1) && !startsSentence(text, found.at(index - 1).start))
               || (index + 1 < found.size() && joined(index, index + 1) && capitalAt(index + 1));
    };
    for (qsizetype at = 0; at < found.size();) {
        const auto candidates = m_first.constFind(folds.at(at));
        qsizetype taken = 0;
        if (candidates != m_first.cend()) {
            for (const Term &term : *candidates) {
                const qsizetype count = term.folded.size();
                if (at + count > found.size())
                    continue;
                const bool properName = proper(m_kinds.value(term.thing));
                bool fits = true;
                for (qsizetype i = 0; i < count && fits; ++i) {
                    const Word &word = found.at(at + i);
                    fits = folds.at(at + i) == term.folded.at(i);
                    if (fits && i > 0) {
                        const Word &before = found.at(at + i - 1);
                        fits = joins(text.mid(before.start + before.length, word.start - before.start - before.length));
                    }
                    // A capital the name has, the page must have, but for a
                    // leading article.
                    if (fits && term.strict && capitalised(term.words.at(i)) && !(i == 0 && count > 1 && article(term.folded.at(0))))
                        fits = capitalised(text.mid(word.start, word.length));
                }
                if (!fits)
                    continue;
                if (count == 1) {
                    // A one-word name that is also an everyday word only
                    // where that word can't be meant.
                    const Word &word = found.at(at);
                    const QString &fold = term.folded.first();
                    const bool ordinary = everyday(fold) || (properName && lower.contains(fold));
                    if (ordinary && (!capitalised(text.mid(word.start, word.length)) || startsSentence(text, word.start)))
                        continue;
                }
                // A part of a name standing in a longer name nobody knows:
                // "Varga" in "Nerys Varga" is not Ilse Varga.
                if (term.part && inName(at))
                    continue;
                const Word &first = found.at(at), &last = found.at(at + count - 1);
                marks.append({first.start, last.start + last.length - first.start, term.thing});
                taken = count;
                break;
            }
        }
        at += taken > 0 ? taken : 1;
    }
    return marks;
}

qint64 Matcher::named(QStringView words) const {
    const QStringList folds = folded(wordsOf(words.toString()));
    if (folds.isEmpty())
        return 0;
    for (const QStringList &tried : {folds, article(folds.first()) ? folds.mid(1) : QStringList{}}) {
        if (tried.isEmpty())
            continue;
        for (const Term &term : m_first.value(tried.first()))
            if (term.folded == tried)
                return term.thing;
    }
    return 0;
}

QList<Suggestion> suggestions(const GraphNode &thing, const GraphPlace &met, const GraphView &view, const GraphPlace &here, int most) {
    QHash<qint64, const GraphNode *> nodes;
    for (const GraphNode &node : view.nodes)
        nodes.insert(node.id, &node);
    // The latest learned first: what matters where the reader is now.
    QList<GraphEdge> edges;
    std::ranges::copy_if(view.edges, std::back_inserter(edges),
                         [&thing](const GraphEdge &edge) { return edge.source == thing.id || edge.target == thing.id; });
    std::ranges::stable_sort(edges, [](const GraphEdge &a, const GraphEdge &b) { return !a.learned.notAfter(b.learned); });

    QList<Suggestion> threads, places, relations;
    for (const GraphEdge &edge : edges) {
        const bool outgoing = edge.source == thing.id;
        const GraphNode *other = nodes.value(outgoing ? edge.target : edge.source);
        if (!other || other->id == thing.id || edge.across())
            continue;
        if (other->kind == "thread")
            threads.append({Suggestion::Shape::Thread, thing.name, other->name, {}});
        else if (other->kind == "place" && outgoing && (proper(thing.kind) || thing.kind == "item"))
            places.append({Suggestion::Shape::Place, thing.name, other->name, {}});
        else if (other->kind == "character" || other->kind == "group" || other->kind == "item") {
            const QString relation = edge.relation().simplified();
            // "mentor of", "member of" read in a question ("Why is Aldo
            // mentor of Ilse?"); "rules" or "commands for" don't, and are
            // asked about as a pair.
            const bool phrase = outgoing && relation.endsWith(" of");
            relations.append(phrase ? Suggestion{Suggestion::Shape::Relation, thing.name, other->name, relation}
                                    : Suggestion{Suggestion::Shape::Between, thing.name, other->name, {}});
        }
    }
    QList<Suggestion> all = threads + places + relations;
    if (met.bookId == here.bookId && met.section < here.section)
        all.append({Suggestion::Shape::Before, thing.name, {}, {}});
    all.append({Suggestion::Shape::Known, thing.name, {}, {}});
    QList<Suggestion> chosen;
    // One of each shape before a second of any, so three suggestions ask three things.
    for (const bool again : {false, true})
        for (const Suggestion &one : all) {
            if (chosen.size() >= most)
                return chosen;
            const bool shapeTaken = std::ranges::any_of(chosen, [&one](const Suggestion &s) { return s.shape == one.shape; });
            if (!chosen.contains(one) && (again || !shapeTaken))
                chosen.append(one);
        }
    return chosen;
}

std::optional<std::pair<int, int>> nearest(const QTextDocument &document, const QString &words, double fraction) {
    QStringList parts;
    for (const QString &word : QString(words).remove(Hyphenation::softHyphen).simplified().split(' ', Qt::SkipEmptyParts))
        parts << QRegularExpression::escape(word);
    if (parts.isEmpty())
        return std::nullopt;
    // Within a paragraph, and blind to soft hyphens: the words are looked
    // for in the text without them, and given where they are shown.
    const QRegularExpression phrase(parts.join("[^\\S\\n]+"), QRegularExpression::CaseInsensitiveOption);
    const Hyphenation::Plain plain(document.toPlainText());
    const double wanted = qBound(0.0, fraction, 1.0) * document.characterCount();
    std::optional<std::pair<int, int>> best;
    double closest = -1;
    for (auto found = phrase.globalMatch(plain.text()); found.hasNext();) {
        const QRegularExpressionMatch match = found.next();
        const auto start = static_cast<int>(plain.toShown(match.capturedStart()));
        const double off = std::abs(start - wanted);
        if (closest < 0 || off < closest) {
            closest = off;
            best = std::pair{start, static_cast<int>(plain.toShownEnd(match.capturedEnd()))};
        }
    }
    return best;
}

} // namespace TextMarks
