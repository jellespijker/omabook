#include "booktext.h"

#include <QtGlobal>
#include <cmath>
#include <utility>

#include "document.h"

BookText::BookText(int sections, std::function<QString(int)> read)
    : m_sections(qMax(0, sections)), m_read(std::move(read)), m_kept(static_cast<size_t>(m_sections)) {}

BookText::BookText(const Document &document) : BookText(document.count(), [&document](int section) { return document.text(section); }) {}

const BookText::Section &BookText::section(int section) const {
    static const Section none{.text = {}, .index = std::make_unique<PassageMatch::Index>(QStringView())};
    if (section < 0 || section >= m_sections)
        return none;
    std::unique_ptr<Section> &kept = m_kept[static_cast<size_t>(section)];
    if (!kept) {
        kept = std::make_unique<Section>(Section{.text = m_read ? m_read(section) : QString(), .index = nullptr});
        kept->index = std::make_unique<PassageMatch::Index>(kept->text);
    }
    return *kept;
}

const QString &BookText::text(int section) const { return this->section(section).text; }

const PassageMatch::Index &BookText::index(int section) const { return *this->section(section).index; }

qsizetype BookText::offsetOf(const BookPlace &place) const {
    const qsizetype length = text(place.section).size();
    return qBound(qsizetype{0}, static_cast<qsizetype>(std::llround(place.fraction * static_cast<double>(length))), length);
}

BookPlace BookText::placeOf(int section, qsizetype offset) const {
    const qsizetype length = text(section).size();
    return {.section = section,
            .fraction = length > 0 ? static_cast<double>(qBound(qsizetype{0}, offset, length)) / static_cast<double>(length) : 0};
}
