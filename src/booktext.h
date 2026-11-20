#pragma once

#include <QString>
#include <functional>
#include <memory>
#include <vector>

#include "bookplace.h"
#include "passagematch.h"

class Document;

// The words of a book, section by section, as Document::text gives them: what
// a passage's places count in (a fraction of a section's text) and where its
// words are sought again (Passage::findAgain). Each section is read when it
// is first asked for, and kept with its index of words, so finding many
// passages in one book reads each section once.
//
// One thread at a time: what is read is kept, which asking from two threads would race on.
class BookText {
public:
    // A book of `sections` sections, each read by `read` when it is needed.
    BookText(int sections, std::function<QString(int)> read);
    // The words of an opened book, which must outlive this.
    explicit BookText(const Document &document);

    [[nodiscard]] int sections() const noexcept { return m_sections; }
    // The text of a section; empty for one the book doesn't have.
    [[nodiscard]] const QString &text(int section) const;
    [[nodiscard]] const PassageMatch::Index &index(int section) const;

    // Where a place is in its section's text, as a character, and the place
    // a character is (BookPlace::fraction is the one over the other).
    [[nodiscard]] qsizetype offsetOf(const BookPlace &place) const;
    [[nodiscard]] BookPlace placeOf(int section, qsizetype offset) const;

private:
    struct Section {
        QString text;
        std::unique_ptr<PassageMatch::Index> index;
    };
    [[nodiscard]] const Section &section(int section) const;

    int m_sections = 0;
    std::function<QString(int)> m_read;
    mutable std::vector<std::unique_ptr<Section>> m_kept;
};
