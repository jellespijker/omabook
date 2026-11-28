// A book's details as the window edits them, and written into its EPUB, the
// library's own copy, when the person asks (#74): beside the window, on a
// library of its own, since a large book's file takes a moment to write.

#include "backend.h"

#include <QtConcurrent>
#include <optional>
#include <tuple>

#include "graph.h"
#include "librarybatch.h"
#include "numberoptions.h"

namespace {

// The fields `details` names, laid over the book's; a field it doesn't name
// stays exactly as it is, not even tidied.
void applyDetails(BookMeta &meta, const QVariantMap &details) {
    const auto line = [&details](const QString &key, QString &field) {
        if (details.contains(key))
            field = details.value(key).toString().simplified();
    };
    line("title", meta.title);
    line("series", meta.series);
    line("publisher", meta.publisher);
    line("language", meta.language);
    line("isbn", meta.isbn);
    line("published", meta.published);
    if (details.contains("authors"))
        meta.authors = BookMeta::splitAuthors(details.value("authors").toString());
    bool number = false;
    if (const double index = details.value("seriesIndex").toDouble(&number); number)
        meta.seriesIndex = index;
    if (details.contains("description"))
        meta.description = details.value("description").toString().trimmed();
    if (details.contains("tags"))
        meta.tags = BookMeta::splitTags(details.value("tags").toString());
}

} // namespace

bool Backend::isDay(const QString &text) const { return Numbers::day(text); }

void Backend::saveDetails(qint64 id, const QVariantMap &details) {
    if (!m_library)
        return;
    {
        // Read and written in one batch, so what another program changes
        // meanwhile isn't written back over (#223).
        const Library::Batch batch(*m_library);
        const auto before = m_library->book(id);
        if (!before)
            return;
        BookMeta meta = before->meta;
        applyDetails(meta, details);
        QString error;
        if (!m_library->setMeta(id, meta, &error))
            say(error);
        if (details.contains("shelf") || details.contains("rating") || details.contains("finished")) {
            QString finished = details.contains("finished") ? details.value("finished").toString().simplified() : before->finished;
            // What is no day changes no day; the rest is saved (#286).
            if (!finished.isEmpty() && finished != before->finished && !isDay(finished)) {
                say(Failure(Failure::Code::FinishedNotADay, {finished}).text());
                finished = before->finished;
            }
            m_library->setReading(id, details.value("shelf", before->shelf).toString(), details.value("rating", before->rating).toInt(),
                                  finished);
        }
        if (details.contains("paper"))
            m_library->setPaper(id, details.value("paper").toBool());
    }
    // The book's node in the knowledge graph says the same: its status,
    // stars, author, series and tags.
    Graph(*m_library).bookNode(id);
    reload();
    // Into its file too, when the edit says so or else the setting does.
    writeDetailsWhenAsked(id, details.value("writeToFile", m_settings->writeDetails()).toBool());
}

QVariantMap Backend::fileDetails(qint64 id) const {
    const auto book = m_library ? m_library->book(id) : std::nullopt;
    return {{"epub", book && book->formats.contains("epub")}, {"original", m_library && !m_library->originalFile(id).isEmpty()}};
}

void Backend::writeDetails(qint64 id) { changeFile(id, false); }

void Backend::restoreFile(qint64 id) { changeFile(id, true); }

void Backend::writeDetailsWhenAsked(qint64 id, bool asked) {
    if (asked && fileDetails(id).value("epub").toBool())
        changeFile(id, false);
}

void Backend::changeFile(qint64 id, bool restore) {
    if (!m_library)
        return;
    // Its end is shown when it comes, unless the window has gone by then.
    std::ignore = QtConcurrent::run([root = m_library->root(), id, restore] -> std::optional<Failure> {
                      Library library(root);
                      if (QString error; !library.open(&error))
                          return Failure::said(error);
                      const auto changed =
                          restore ? library.restoreFile(id) : library.writeDetails(id).transform([](const Library::DetailsWritten &) {});
                      return changed ? std::nullopt : std::optional(changed.error());
                  }).then(this, [this](const std::optional<Failure> &failure) {
        if (failure)
            say(failure->text());
        reload();
    });
}
