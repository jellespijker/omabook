#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <array>
#include <span>
#include <utility>

#include "library.h"

// The search's language (Book::matches): the words a search is made of, the
// fields it looks in, the genres it knows, and what is wrong with one as a
// collection's rule. What a search finds past a book's own details is asked
// of the lookups it is given: what is known of the book (knows:, the
// knowledge graph) and what the reader wrote in it (notes:, #115).

using namespace Qt::StringLiterals;

namespace {

// The words of a search, a quoted phrase as one: `tag:"science fiction"`
// is one word, and so is `"the hobbit"`.
[[nodiscard]] QStringList searchWords(const QString &query, bool *open = nullptr) {
    QStringList words;
    QString word;
    bool quoted = false;
    for (const QChar c : query) {
        if (c == '"') {
            quoted = !quoted;
        } else if (c.isSpace() && !quoted) {
            if (!word.isEmpty())
                words << std::exchange(word, {});
        } else {
            word += c;
        }
    }
    if (!word.isEmpty())
        words << word;
    if (open)
        *open = quoted;
    return words;
}

// What tags say a book is a story, and what tags say it is about the world.
// A tag that is both ("Historical Fiction", "Science Fiction") is a story.
constexpr std::array storyWords{"fiction"_L1,    "novel"_L1,    "fantasy"_L1, "sci-fi"_L1,        "mystery"_L1,   "mysteries"_L1,
                                "thriller"_L1,   "romance"_L1,  "horror"_L1,  "crime"_L1,         "adventure"_L1, "short stories"_L1,
                                "fairy tale"_L1, "dystopia"_L1, "fables"_L1,  "graphic novel"_L1, "detective"_L1};
constexpr std::array worldWords{"nonfiction"_L1,  "non-fiction"_L1, "history"_L1,   "biography"_L1,  "memoir"_L1,     "science"_L1,
                                "programming"_L1, "computer"_L1,    "software"_L1,  "technology"_L1, "philosophy"_L1, "business"_L1,
                                "economics"_L1,   "reference"_L1,   "textbook"_L1,  "self-help"_L1,  "psychology"_L1, "mathematics"_L1,
                                "politics"_L1,    "essays"_L1,      "travel"_L1,    "cooking"_L1,    "health"_L1,     "education"_L1,
                                "engineering"_L1, "management"_L1,  "design"_L1,    "language"_L1,   "religion"_L1,   "law"_L1,
                                "nature"_L1,      "cookbook"_L1,    "parenting"_L1, "medicine"_L1};

[[nodiscard]] bool tagHas(const QString &tag, std::span<const QLatin1StringView> words) {
    return std::ranges::any_of(words, [&tag](QLatin1StringView word) { return tag.contains(word, Qt::CaseInsensitive); });
}

[[nodiscard]] bool storyTag(const QString &tag) {
    return tagHas(tag, storyWords) && !tag.contains("nonfiction"_L1, Qt::CaseInsensitive)
           && !tag.contains("non-fiction"_L1, Qt::CaseInsensitive);
}

[[nodiscard]] bool genreOf(const QStringList &tags, const QString &genre) {
    const QString wanted = genre.toLower().remove(' ');
    if (wanted == "fiction"_L1)
        return std::ranges::any_of(tags, storyTag);
    if (wanted == "nonfiction"_L1 || wanted == "non-fiction"_L1)
        return std::ranges::any_of(tags, [](const QString &tag) { return tagHas(tag, worldWords) && !storyTag(tag); });
    return std::ranges::any_of(tags, [&genre](const QString &tag) { return tag.contains(genre, Qt::CaseInsensitive); });
}

// Whether a book is held as a search's "is:" says: digital, on paper, or only wished for.
[[nodiscard]] bool isHeld(const Book &book, const QString &value) {
    const QString wanted = value.toLower();
    if (wanted == "digital"_L1)
        return !book.formats.isEmpty();
    if (wanted == "paper"_L1)
        return book.paper;
    return wanted == "wishlist"_L1 && book.held() == "none"_L1;
}

[[nodiscard]] bool hasStatus(const Book &book, const QString &value) {
    const QString wanted = value.toLower();
    if (wanted == "unread"_L1)
        return book.status() != "read"_L1 && book.status() != "reading"_L1;
    return book.status() == wanted;
}

// The fields a search can look in one at a time, as "key:value", and how
// each is looked in: whether the book's holds the value.
struct Field {
    QLatin1StringView key;
    bool (*holds)(const Book &book, const QString &value, const Book::Lookups &lookups);
};
constexpr std::array fields{
    Field{"author"_L1, [](const Book &book, const QString &value,
                          const Book::Lookups &) { return book.meta.authorText().contains(value, Qt::CaseInsensitive); }},
    Field{"title"_L1, [](const Book &book, const QString &value,
                         const Book::Lookups &) { return book.meta.title.contains(value, Qt::CaseInsensitive); }},
    Field{"series"_L1, [](const Book &book, const QString &value,
                          const Book::Lookups &) { return book.meta.series.contains(value, Qt::CaseInsensitive); }},
    Field{"tag"_L1, [](const Book &book, const QString &value,
                       const Book::Lookups &) { return book.meta.tags.join("\n").contains(value, Qt::CaseInsensitive); }},
    Field{"genre"_L1, [](const Book &book, const QString &value, const Book::Lookups &) { return genreOf(book.meta.tags, value); }},
    Field{"format"_L1,
          [](const Book &book, const QString &value, const Book::Lookups &) { return book.formats.contains(value.toLower()); }},
    Field{"lang"_L1, [](const Book &book, const QString &value,
                        const Book::Lookups &) { return book.meta.language.startsWith(value, Qt::CaseInsensitive); }},
    Field{"publisher"_L1, [](const Book &book, const QString &value,
                             const Book::Lookups &) { return book.meta.publisher.contains(value, Qt::CaseInsensitive); }},
    Field{"about"_L1, [](const Book &book, const QString &value,
                         const Book::Lookups &) { return book.meta.description.contains(value, Qt::CaseInsensitive); }},
    Field{"status"_L1, [](const Book &book, const QString &value, const Book::Lookups &) { return hasStatus(book, value); }},
    Field{"is"_L1, [](const Book &book, const QString &value, const Book::Lookups &) { return isHeld(book, value); }},
    Field{"rating"_L1,
          [](const Book &book, const QString &value, const Book::Lookups &) { return book.rating >= value.toInt() && book.rating > 0; }},
    // What is known of the book, as far as it was read (the knowledge graph), and nothing else.
    Field{"knows"_L1, [](const Book &book, const QString &value,
                         const Book::Lookups &lookups) { return lookups.knows && lookups.knows(book, value, true); }},
    // What the reader wrote in it (#115).
    Field{"notes"_L1,
          [](const Book &book, const QString &value, const Book::Lookups &lookups) { return lookups.noted && lookups.noted(book, value); }},
};

[[nodiscard]] const Field *fieldNamed(QStringView key) {
    const auto *const found =
        std::ranges::find_if(fields, [&key](const Field &field) { return key.compare(field.key, Qt::CaseInsensitive) == 0; });
    return found == fields.end() ? nullptr : &*found;
}

} // namespace

bool Book::matches(const QString &query) const { return matches(query, Lookups{}); }

bool Book::matches(const QString &query, const Knows &knows) const { return matches(query, Lookups{.knows = knows, .noted = {}}); }

bool Book::matches(const QString &query, const Lookups &lookups) const {
    const QString everything = meta.title + "\n" + meta.authorText() + "\n" + meta.series + "\n" + meta.tags.join("\n");
    const auto found = [this, &everything, &lookups](const QString &word) {
        if (const Field *field = word.contains(':') ? fieldNamed(word.section(':', 0, 0)) : nullptr)
            return field->holds(*this, word.section(':', 1), lookups);
        return everything.contains(word, Qt::CaseInsensitive) || (lookups.knows && lookups.knows(*this, word, false));
    };
    // A word with a minus in front is one the book must not match.
    const auto holds = [&found](const QString &word) {
        return word.size() > 1 && word.startsWith('-') ? !found(word.mid(1)) : found(word);
    };
    // Words joined by OR are alternatives: any one group of words, each of
    // which matches, is enough.
    QList<QStringList> groups{{}};
    for (const QString &word : searchWords(query)) {
        if (word == "OR"_L1)
            groups.append(QStringList{});
        else
            groups.last() << word;
    }
    groups.removeIf([](const QStringList &group) { return group.isEmpty(); });
    return groups.isEmpty()
           || std::ranges::any_of(groups, [&holds](const QStringList &group) { return std::ranges::all_of(group, holds); });
}

QString Book::searchProblem(const QString &query) {
    bool open = false;
    const QStringList words = searchWords(query, &open);
    if (open)
        return "A quote is left open; close it with another \".";
    static const QRegularExpression keyed(u"^-?([A-Za-z]+):"_s);
    for (const QString &word : words) {
        const QRegularExpressionMatch match = keyed.match(word);
        const QString key = match.captured(1).toLower();
        // What is known of a book grows as it is read, and a rule holds the
        // same books for everyone who opens the library.
        if (match.hasMatch() && key == "knows"_L1)
            return "A collection's rule goes by the books' own details; knows: is for the search, where what you have read counts.";
        if (match.hasMatch() && key == "notes"_L1)
            return "A collection's rule goes by the books' own details; notes: is for the search, where what you wrote counts.";
        if (match.hasMatch() && !fieldNamed(key)) {
            QStringList keys;
            for (const Field &field : fields)
                if (field.key != "knows"_L1 && field.key != "notes"_L1)
                    keys << field.key + ':';
            return QString("\"%1:\" isn't a field a search looks in; it can be %2.").arg(match.captured(1), keys.join(", "));
        }
    }
    return {};
}
