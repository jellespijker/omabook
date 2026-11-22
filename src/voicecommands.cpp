#include "voicecommands.h"

#include <QRegularExpression>
#include <algorithm>
#include <chrono>
#include <vector>

#include "ai.h"
#include "aicall.h"
#include "commandtable.h"

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace {

// A model that hasn't answered by then won't be waited for: the person is
// reading, and says it again or presses the key.
constexpr std::chrono::seconds patience = 20s;

[[nodiscard]] QString whatItDoes(const QString &command) {
    if (command == QLatin1StringView(VoiceGrammar::chapter))
        return u"Go to the chapter of that number; answer with the number after a space (\"spoken.chapter 5\")"_s;
    if (command == QLatin1StringView(VoiceGrammar::highlight))
        return u"Highlight the selected words"_s;
    if (command == QLatin1StringView(VoiceGrammar::meaning))
        return u"Say what a word means: the selected one, or the one said, after a space (\"spoken.meaning serendipity\")"_s;
    const auto *const found = std::ranges::find_if(CommandTable::builtIns, [&](const CommandTable::BuiltIn &one) {
        return QLatin1StringView(one.id.data(), static_cast<qsizetype>(one.id.size())) == command;
    });
    return found == CommandTable::builtIns.end() ? QString() : QString::fromUtf8(found->what);
}

} // namespace

VoiceCommands::VoiceCommands(Answerer answerer, QObject *parent) : QObject(parent), m_answerer(std::move(answerer)) {
    m_patience.setSingleShot(true);
    m_patience.setInterval(patience);
    connect(&m_patience, &QTimer::timeout, this, [this] {
        const QString said = m_said;
        cancel();
        emit notUnderstood(said);
    });
}

VoiceCommands::~VoiceCommands() { cancel(); }

QString VoiceCommands::instructions() {
    QString text = u"Someone reading a book in Omabook said something to it, which is given to you as it was heard. Answer with "
                   "the one command below that it asks for, and nothing else: its id, and for the two that take one, what goes "
                   "with it after a space. Where it asks for none of them, or you can't tell, answer: none\n\n"_s;
    for (const QString &command : VoiceGrammar::commands())
        text += command + u": "_s + whatItDoes(command) + u'\n';
    return text;
}

std::optional<VoiceGrammar::Heard> VoiceCommands::parse(const QString &answer) {
    // The first line, without the quotes or marks a model may put round it.
    const QString line = answer.trimmed().section(u'\n', 0, 0).remove(QRegularExpression(u"[`*\"“”]"_s)).trimmed();
    const QString command = line.section(u' ', 0, 0).remove(QRegularExpression(u"[.,;:]$"_s));
    const QString rest = line.section(u' ', 1).trimmed();
    if (!VoiceGrammar::commands().contains(command))
        return std::nullopt;
    VoiceGrammar::Heard heard{.command = command, .number = 0, .words = {}, .language = {}};
    if (command == QLatin1StringView(VoiceGrammar::chapter)) {
        bool figures = false;
        heard.number = rest.section(u' ', 0, 0).toInt(&figures);
        if (!figures || heard.number <= 0)
            return std::nullopt;
    }
    if (command == QLatin1StringView(VoiceGrammar::meaning))
        heard.words = rest;
    return heard;
}

void VoiceCommands::understand(const QString &said, const QString &language) {
    cancel();
    const QString words = said.simplified();
    if (words.isEmpty()) {
        emit notUnderstood(words);
        return;
    }
    if (const auto heard = VoiceGrammar::understand(words, language)) {
        emit understood(heard->command, heard->number, heard->words, words);
        return;
    }
    ask(words);
}

void VoiceCommands::ask(const QString &said) {
    QString why;
    std::unique_ptr<AiProvider> provider = m_answerer ? m_answerer(&why) : nullptr;
    if (!provider) {
        // No model may be asked (the AI is off, or none is chosen): the grammar's word is the last.
        emit notUnderstood(said);
        return;
    }
    m_call = std::make_unique<AiCall>(std::move(provider));
    m_said = said;
    connect(m_call.get(), &AiCall::done, this, [this, said](const QString &answer) { answered(said, answer); });
    connect(m_call.get(), &AiCall::failed, this, [this, said] {
        ended();
        emit notUnderstood(said);
    });
    m_patience.start();
    emit askingChanged();
    m_call->ask(
        {.system = instructions(), .messages = {{.role = u"user"_s, .text = said}}, .tier = AiRequest::Quick, .model = {}, .prose = false});
}

void VoiceCommands::answered(const QString &said, const QString &answer) {
    ended();
    if (const auto heard = parse(answer))
        emit understood(heard->command, heard->number, heard->words, said);
    else
        emit notUnderstood(said);
}

void VoiceCommands::ended() {
    m_patience.stop();
    if (!m_call)
        return;
    // Let go of once its provider is done telling: this may be in its signal.
    m_call.release()->deleteLater();
    emit askingChanged();
}

void VoiceCommands::cancel() {
    m_patience.stop();
    if (!m_call)
        return;
    m_call->disconnect(this);
    m_call->cancel();
    ended();
}

int VoiceCommands::chapter(const QVariantList &contents, int number) const {
    std::vector<VoiceGrammar::Entry> entries;
    entries.reserve(static_cast<std::size_t>(contents.size()));
    for (const QVariant &entry : contents) {
        const QVariantMap fields = entry.toMap();
        entries.push_back({.title = fields.value(u"title"_s).toString(), .level = fields.value(u"level"_s).toInt()});
    }
    return VoiceGrammar::chapterIn(entries, number);
}
