#include "pageformat.h"

#include <QFontDatabase>
#include <algorithm>

#include "library.h"
#include "reader.h"
#include "readingformat.h"
#include "settings.h"

PageFormat::PageFormat(Reader *reader, Settings *settings, QObject *parent)
    : QObject(parent), m_reader(reader), m_settings(settings) {
    connect(settings, &Settings::changed, this, &PageFormat::refresh);
    connect(reader, &Reader::bookChanged, this, &PageFormat::refresh);
    connect(reader, &Reader::htmlChanged, this, &PageFormat::restyle);
    refresh();
}

bool PageFormat::hasBook() const { return m_reader->bookId() > 0 && m_reader->library(); }

QVariantMap PageFormat::bounds() const { return ReadingFormat::bounds().toVariantMap(); }

QStringList PageFormat::fonts() const {
    static const QStringList families = [] {
        QStringList families = QFontDatabase::families();
        families.removeIf([](const QString &family) { return QFontDatabase::isPrivateFamily(family); });
        return families;
    }();
    return families;
}

void PageFormat::setLinkColor(const QColor &color) {
    if (color == m_links)
        return;
    m_links = color;
    m_html.clear();
    restyle();
}

void PageFormat::refresh() {
    const QJsonObject defaults = m_settings->format();
    const QJsonObject own = hasBook() ? m_reader->library()->bookFormat(m_reader->bookId()) : QJsonObject();
    if (defaults == m_defaults && own == m_own && !m_effective.isEmpty())
        return;
    m_defaults = defaults;
    m_own = own;
    m_effective = ReadingFormat::over(defaults, own);
    emit changed();
    m_html.clear();
    restyle();
}

void PageFormat::restyle() {
    const QString chapter = m_reader->html();
    if (!m_html.isEmpty() && chapter == m_chapter)
        return;
    m_chapter = chapter;
    m_html = ReadingFormat::setChapter(chapter, m_effective, m_links);
    emit htmlChanged();
}

QString PageFormat::set(const QString &key, const QVariant &value, bool forBook) {
    const QJsonValue json = QJsonValue::fromVariant(value);
    // A number from QML may come as a double that is a whole one.
    const QJsonValue given = json.isDouble() ? QJsonValue(qRound64(json.toDouble())) : json;
    if (const QString why = ReadingFormat::problem(key, given); !why.isEmpty())
        return why;
    if (!forBook) {
        m_settings->setOption(key, given);
        return {};
    }
    if (!hasBook())
        return "No book is open to give it its own.";
    QJsonObject own = m_own;
    own.insert(key, given);
    if (!m_reader->library()->setBookFormat(m_reader->bookId(), own))
        return "The library can't be written.";
    refresh();
    return {};
}

void PageFormat::resetBook() {
    if (!hasBook() || m_own.isEmpty())
        return;
    m_reader->library()->setBookFormat(m_reader->bookId(), {});
    refresh();
}

QVariantMap PageFormat::snapshot() const {
    return {{"defaults", m_defaults.toVariantMap()}, {"own", m_own.toVariantMap()}, {"book", m_reader->bookId()}};
}

void PageFormat::restore(const QVariantMap &snapshot) {
    const QJsonObject defaults = QJsonObject::fromVariantMap(snapshot.value("defaults").toMap());
    for (auto it = defaults.begin(); it != defaults.end(); ++it)
        m_settings->setOption(it.key(), it.value());
    if (hasBook() && snapshot.value("book").toLongLong() == m_reader->bookId())
        m_reader->library()->setBookFormat(m_reader->bookId(), QJsonObject::fromVariantMap(snapshot.value("own").toMap()));
    refresh();
}

void PageFormat::zoom(int steps) {
    const bool forBook = m_own.contains("font_size");
    const ReadingFormat::Range *size = ReadingFormat::range(u"font_size");
    set("font_size", std::clamp(m_effective.value("font_size").toInt(size->fallback) + steps, size->low, size->high), forBook);
}
