#include "readingselection.h"

#include <memory>
#include <optional>

#include "backend.h"
#include "bookplace.h"
#include "booktext.h"
#include "document.h"
#include "reader.h"
#include "scriptplugins.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

ReadingSelection::ReadingSelection(Reader *reader) : QObject(reader), m_reader(reader) {
    // Told once it has settled, not on every character the selection grows by.
    m_settle.setSingleShot(true);
    m_settle.setInterval(300);
    connect(&m_settle, &QTimer::timeout, this, &ReadingSelection::tell);
}

void ReadingSelection::changed(const QString &words) {
    m_words = words.simplified();
    m_settle.start();
}

QJsonObject ReadingSelection::passage(const QString &words) const {
    const std::shared_ptr<const Document> document = m_reader->document();
    const Library *library = m_reader->library();
    if (words.simplified().isEmpty() || !document || !library || m_reader->bookId() <= 0)
        return {};
    // Where the reader is, which the words are near: they are sought there first.
    const BookPlace near{.section = m_reader->section(), .fraction = m_reader->fraction()};
    const Passage chosen{.from = near, .to = near, .quote = words.simplified(), .before = {}, .after = {}};
    const std::optional<FoundPassage> found = chosen.findAgain(BookText(*document));
    if (!found)
        return {};
    return {{"book", m_reader->bookId()},
            {"global_id", Spaces::bookId(*library, m_reader->bookId())},
            {"passage", found->passage.toJson()},
            {"confidence", found->confidence}};
}

void ReadingSelection::tell() {
    if (ScriptPlugins *scripts = m_reader->backend() ? m_reader->backend()->scripts() : nullptr)
        scripts->setSelection(passage(m_words));
}
