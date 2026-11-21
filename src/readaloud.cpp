#include "readaloud.h"

#include <QJsonObject>
#include <QRegularExpression>
#include <QTextDocument>
#include <algorithm>
#include <cmath>

#include "backend.h"
#include "pagetext.h"
#include "reader.h"
#include "settings.h"
#include "textgeometry.h"
#include "voices.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QVariantList variants(const QList<QRectF> &rects) {
    QVariantList list;
    for (const QRectF &rect : rects)
        list << rect;
    return list;
}

// The words at the top of a page as a pattern that finds them in the
// book's text, whatever stands between them there. The first may be cut
// off at the edge of the page, so it is left out when there are more.
[[nodiscard]] QRegularExpression wordsPattern(const QString &atTop) {
    static const QRegularExpression notWord(u"[^\\w]+"_s, QRegularExpression::UseUnicodePropertiesOption);
    QStringList words = atTop.split(notWord, Qt::SkipEmptyParts);
    if (words.size() > 2)
        words.removeFirst();
    words = words.mid(0, 6);
    for (QString &word : words)
        word = QRegularExpression::escape(word);
    return QRegularExpression(words.isEmpty() ? QString() : u"\\b"_s + words.join(u"\\W+"_s),
                              QRegularExpression::UseUnicodePropertiesOption);
}

} // namespace

ReadAloud::ReadAloud(Reader *reader) : QObject(reader), m_reader(reader) {
    connect(m_reader, &Reader::bookChanged, this, [this] {
        if (m_reader->bookId() != m_bookId)
            stop();
    });
    // The person went to another chapter or page while it was being read: on from there.
    connect(m_reader, &Reader::sectionChanged, this, [this] {
        const auto now = current();
        if (m_following || !now || now->section == m_reader->section())
            return;
        if (!m_narration->from(m_reader->section(), 0))
            stop();
    });
}

ReadAloud::~ReadAloud() = default;

void ReadAloud::setVoiceMaker(VoiceMaker maker, QString name) {
    m_maker = std::move(maker);
    m_makerName = std::move(name);
    refresh();
}

int ReadAloud::section() const {
    const auto now = current();
    return now ? now->section : -1;
}

std::optional<Narration::Spoken> ReadAloud::current() const { return m_narration ? m_narration->current() : std::nullopt; }

QString ReadAloud::voice() const { return m_reader->backend()->settings()->readAloud().value("voice"_L1).toString(); }

void ReadAloud::setVoice(const QString &voice) { choose(u"voice"_s, voice); }

double ReadAloud::rate() const { return Voices::speech(*m_reader->backend()->settings(), {}).rate; }

void ReadAloud::setRate(double rate) { choose(u"rate"_s, std::round(std::clamp(rate, 0.5, 2.0) * 10) / 10); }

void ReadAloud::choose(const QString &key, const QJsonValue &value) {
    Settings &settings = *m_reader->backend()->settings();
    QJsonObject chosen = settings.readAloud();
    if (chosen.value(key) == value)
        return;
    chosen.insert(key, value);
    settings.setReadAloud(chosen);
    if (m_narration) {
        const std::shared_ptr<const Document> document = m_reader->document();
        m_narration->setSpeech(Voices::speech(settings, document ? document->meta().language : QString()));
    }
    emit choicesChanged();
}

void ReadAloud::refresh() {
    const Settings &settings = *m_reader->backend()->settings();
    if (m_maker) {
        m_engine = m_makerName;
        m_why.clear();
        m_voices.clear();
    } else if (const auto engine = Voices::engine(settings)) {
        m_engine = *engine == "piper"_L1 ? u"Piper"_s : *engine;
        m_why.clear();
        m_voices = Voices::offered(settings);
    } else {
        m_engine.clear();
        m_why = engine.error().text();
        m_voices.clear();
    }
    emit choicesChanged();
}

qsizetype ReadAloud::offsetOf(int section, const QString &atTop) const {
    const std::shared_ptr<const Document> document = m_reader->document();
    const QRegularExpression pattern = wordsPattern(atTop);
    if (!document || pattern.pattern().isEmpty())
        return -1;
    const QRegularExpressionMatch found = pattern.match(document->text(section));
    return found.hasMatch() ? found.capturedStart() : -1;
}

void ReadAloud::toggle(const QString &atTop) {
    if (!m_narration)
        start(atTop);
    else if (m_narration->playing())
        pause();
    else
        resume();
}

void ReadAloud::start(const QString &atTop) {
    stop();
    const std::shared_ptr<const Document> document = m_reader->document();
    if (!document)
        return;
    auto voice = m_maker ? m_maker() : Voices::choose(*m_reader->backend()->settings());
    if (!voice) {
        failed(voice.error());
        return;
    }
    const int section = m_reader->section();
    qsizetype offset = 0;
    if (document->kind() == Document::Flow) {
        offset = offsetOf(section, atTop);
        if (offset < 0)
            offset = static_cast<qsizetype>(m_reader->fraction() * static_cast<double>(document->text(section).size()));
    }
    // A PDF's page breaks its lines where they were full, inside its sentences.
    const auto lines = document->kind() == Document::Pages ? Sentences::Lines::Wrap : Sentences::Lines::End;
    auto narration =
        std::make_unique<Narration>(document->count(), [document](int at) { return document->text(at); }, *std::move(voice), lines);
    narration->setSpeech(Voices::speech(*m_reader->backend()->settings(), document->meta().language));
    if (!narration->from(section, offset)) {
        failed(Failure(Failure::Code::NothingToReadAloud));
        return;
    }
    m_narration = std::move(narration);
    m_bookId = m_reader->bookId();
    connect(m_narration.get(), &Narration::moved, this, &ReadAloud::moved);
    connect(m_narration.get(), &Narration::playingChanged, this, &ReadAloud::stateChanged);
    connect(m_narration.get(), &Narration::failed, this, &ReadAloud::failed);
    connect(m_narration.get(), &Narration::finished, this, [this] { stop(); });
    moved();
    m_narration->play();
    emit stateChanged();
}

void ReadAloud::pause() {
    if (m_narration)
        m_narration->pause();
}

void ReadAloud::resume() {
    if (!m_narration)
        return;
    // Paused, the reader may have gone elsewhere: the sentence is shown again as it goes on.
    moved();
    m_narration->play();
}

void ReadAloud::stop() {
    if (!m_narration)
        return;
    // Silent at once, and gone after the signal it may have come from.
    m_narration->disconnect(this);
    m_narration->pause();
    m_narration.release()->deleteLater();
    m_bookId = 0;
    emit sentenceChanged();
    emit stateChanged();
}

void ReadAloud::next() {
    if (m_narration)
        m_narration->next();
}

void ReadAloud::back() {
    if (m_narration)
        m_narration->back();
}

void ReadAloud::moved() {
    ++m_sentences;
    if (const auto now = current(); now && now->section != m_reader->section()) {
        m_following = true;
        m_reader->goToPlace(now->section, m_narration->place().fraction);
        m_following = false;
    }
    emit sentenceChanged();
}

void ReadAloud::failed(const Failure &why) {
    stop();
    m_reader->backend()->say(why.text());
}

QVariantList ReadAloud::rectsIn(QQuickTextDocument *chapter) const {
    const auto now = current();
    const QTextDocument *document = chapter ? chapter->textDocument() : nullptr;
    if (!now || !document || now->section != m_reader->section())
        return {};
    const auto span = m_narration->passage().spanIn(document->toPlainText());
    return span ? variants(TextGeometry::rects(*document, static_cast<int>(span->first), static_cast<int>(span->second))) : QVariantList();
}

QVariantList ReadAloud::rectsOnPage(int page, QSizeF shown) const {
    const auto now = current();
    const std::shared_ptr<const Document> document = m_reader->document();
    if (!now || !document || now->section != page)
        return {};
    return variants(
        PageText::rects(document->words(page), static_cast<int>(now->sentence.start), static_cast<int>(now->sentence.end), shown));
}
