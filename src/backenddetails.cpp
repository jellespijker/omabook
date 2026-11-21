// A book's details as the window edits them, and written into its EPUB, the
// library's own copy, when the person asks (#74): beside the window, on a
// library of its own, since a large book's file takes a moment to write.

#include "backend.h"

#include <QtConcurrent>
#include <optional>
#include <tuple>

#include "graph.h"

void Backend::saveDetails(qint64 id, const QVariantMap &details) {
    const auto before = m_library ? m_library->book(id) : std::nullopt;
    if (!before)
        return;
    BookMeta meta = before->meta;
    meta.title = details.value("title", meta.title).toString().simplified();
    meta.authors = BookMeta::splitAuthors(details.value("authors", meta.authorText()).toString());
    meta.series = details.value("series", meta.series).toString().simplified();
    meta.seriesIndex = details.value("seriesIndex", meta.seriesIndex).toDouble();
    meta.publisher = details.value("publisher", meta.publisher).toString().simplified();
    meta.language = details.value("language", meta.language).toString().simplified();
    meta.isbn = details.value("isbn", meta.isbn).toString().simplified();
    meta.published = details.value("published", meta.published).toString().simplified();
    meta.description = details.value("description", meta.description).toString().trimmed();
    meta.tags = BookMeta::splitTags(details.value("tags", meta.tags.join(", ")).toString());
    QString error;
    if (!m_library->setMeta(id, meta, &error))
        say(error);
    if (details.contains("shelf"))
        m_library->setReading(id, details.value("shelf").toString(), details.value("rating", before->rating).toInt(),
                              details.value("finished", before->finished).toString().simplified());
    if (details.contains("paper"))
        m_library->setPaper(id, details.value("paper").toBool());
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
    std::ignore = QtConcurrent::run([root = m_library->root(), id, restore]() -> std::optional<Failure> {
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
