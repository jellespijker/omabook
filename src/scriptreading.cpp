#include "scriptreading.h"

#include <QFile>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <memory>
#include <optional>

#include "bookplace.h"
#include "bookspaces.h"
#include "booktext.h"
#include "document.h"
#include "formats.h"
#include "library.h"
#include "scriptplugin.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QString compact(const QJsonValue &value) {
    return QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact)).mid(1).chopped(1);
}

[[nodiscard]] QJsonObject parsed(const QString &json) { return QJsonDocument::fromJson(json.toUtf8()).object(); }

// The book's text, as a passage's places count it: of the first of its files Omabook reads itself.
[[nodiscard]] std::unique_ptr<Document> opened(const Library &library, const Book &book) {
    for (const QString &format : book.formats)
        if (Formats::readable(format))
            return Formats::open(library.formatPath(book.id, format));
    return nullptr;
}

// What a plugin hands make(): a passage, a selection ({book, passage}), or a place, with words chosen there or none.
[[nodiscard]] Passage passageOf(const BookText &text, const QJsonObject &given) {
    const QJsonObject passage = given.contains("passage"_L1) ? given.value("passage"_L1).toObject() : given;
    if (passage.contains("from"_L1)) {
        Passage said = Passage::fromJson(passage);
        // Its words placed in the book's text, where they are now.
        if (const std::optional<FoundPassage> found = said.findAgain(text))
            return found->passage;
        return said;
    }
    const BookPlace place = BookPlace::fromJson(passage);
    const QString words = passage.value("quote"_L1).toString().simplified();
    if (words.isEmpty())
        return Passage::at(text, place);
    const Passage chosen{.from = place, .to = place, .quote = words, .before = {}, .after = {}};
    const std::optional<FoundPassage> found = chosen.findAgain(text);
    return found ? found->passage : Passage{};
}

} // namespace

QJsonObject ScriptBook::of(const Library *library, const Book &book, bool whole) {
    QJsonObject object = book.toJson();
    // Book::toJson gives `generated` and `identifiers` always, as the command line does (#128).
    object.insert("has_cover", book.hasCover);
    if (!library)
        return object;
    object.insert("global_id", Spaces::bookId(*library, book.id));
    if (whole) {
        QJsonObject files;
        for (const QString &format : book.formats)
            files.insert(format, library->formatPath(book.id, format));
        object.insert("files", files);
        object.insert("cover", library->coverPath(book.id));
    }
    return object;
}

QJSValue ScriptReading::build(QJSEngine &engine) {
    QFile wrapper(u":/scriptreading.js"_s);
    if (!wrapper.open(QIODevice::ReadOnly))
        return engine.newErrorObject(QJSValue::GenericError, u"Omabook's own side of omabook.passages is missing."_s);
    QJSEngine::setObjectOwnership(this, QJSEngine::CppOwnership);
    return engine.evaluate(QString::fromUtf8(wrapper.readAll()), u"omabook-api/reading"_s).call({engine.newQObject(this)});
}

Library *ScriptReading::library() const { return m_plugin->m_services.library ? m_plugin->m_services.library() : nullptr; }

void ScriptReading::fail(const QString &why) { m_plugin->m_engine.throwError(why); }

QString ScriptReading::selection() const {
    const QJsonObject selected = m_plugin->m_services.selection ? m_plugin->m_services.selection() : QJsonObject();
    return selected.isEmpty() ? u"null"_s : compact(selected);
}

QString ScriptReading::make(double book, const QString &given) {
    const Library *library = this->library();
    const std::optional<Book> found = library ? library->book(static_cast<qint64>(book)) : std::nullopt;
    if (!found) {
        fail(u"There is no book %1 in the library."_s.arg(static_cast<qint64>(book)));
        return {};
    }
    const std::unique_ptr<Document> document = opened(*library, *found);
    if (!document) {
        fail(u"Book %1 has no file Omabook reads itself, so it has no passages."_s.arg(found->id));
        return {};
    }
    const QJsonObject asked = parsed(given);
    const BookText text(*document);
    const Passage passage = passageOf(text, asked);
    if (passage.quote.isEmpty() && !asked.value("quote"_L1).toString().simplified().isEmpty()) {
        fail(u"Those words aren't in the book near that place."_s);
        return {};
    }
    return compact(passage.toJson());
}

QString ScriptReading::find(double book, const QString &passage) {
    const Library *library = this->library();
    const std::optional<Book> found = library ? library->book(static_cast<qint64>(book)) : std::nullopt;
    const std::unique_ptr<Document> document = found ? opened(*library, *found) : nullptr;
    if (!document)
        return u"null"_s;
    const std::optional<FoundPassage> again = Passage::fromJson(parsed(passage)).findAgain(BookText(*document));
    return again ? compact(QJsonObject{{"passage", again->passage.toJson()}, {"confidence", again->confidence}}) : u"null"_s;
}

QString ScriptReading::byGlobalId(const QString &id) {
    const Library *library = this->library();
    const qint64 here = library && !id.isEmpty() ? Spaces::localId(*library, u"book"_s, id) : 0;
    const std::optional<Book> book = here > 0 ? library->book(here) : std::nullopt;
    return book ? compact(ScriptBook::of(library, *book)) : u"null"_s;
}

QString ScriptReading::sharedIn(double book) {
    QJsonArray spaces;
    if (const Library *library = this->library())
        for (const BookSpaces::Held &held : BookSpaces::of(*library, static_cast<qint64>(book)))
            spaces << QJsonObject{{"space", held.space}, {"kind", held.kind}, {"name", held.name}, {"collection", held.collection}};
    return compact(spaces);
}
