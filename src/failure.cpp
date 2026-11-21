#include "failure.h"

#include <QCoreApplication>
#include <algorithm>
#include <array>

#include "translator.h"

using namespace Qt::StringLiterals;

namespace {

// Marks a sentence for the catalogs (bin/i18n reads what tr() is handed) and
// gives it back as it is; text() translates it when it is shown. A counted
// one is marked tr("...", "", counted), and its %n is the failure's count.
constexpr auto tr = [](const char *text, const char * = nullptr, bool = false) { return text; };
constexpr bool counted = true;

struct Wording {
    Failure::Code code;
    const char *text;
};

// Every code's sentence, by code. %1, %2... are its arguments, in order.
constexpr auto wordings = std::to_array<Wording>({
    {Failure::Code::NoSuchFile, tr("There is no such file: %1")},
    {Failure::Code::FileUnreadable, tr("The file can't be read: %1")},
    {Failure::Code::FileWithoutText, tr("The file has no text.")},
    {Failure::Code::NoChaptersFound, tr("%1 found no chapters in %2.")},
    {Failure::Code::UnknownFolder, tr("Omabook doesn't know what kind of book the folder %1 is.")},
    {Failure::Code::UnknownFormat, tr("Omabook doesn't know the format of %1.")},
    {Failure::Code::FormatOfMissingPlugin,
     tr("%1 is a %2, which the %3 plugin reads, and it isn't installed here. Install it: omabook plugin add %4")},
    {Failure::Code::OpenedInItsView, tr("%1 is a %2, which its plugin opens in a view of its own rather than the reader.")},
    {Failure::Code::FormatOfIdlePlugin, tr("%1 is a %2, which the %3 plugin opens, and it isn't running. Switch it on under Plugins.")},
    {Failure::Code::ReadAfterConverting, tr("%1 files are read by converting them to EPUB first.")},
    {Failure::Code::EpubNotAnArchive, tr("Not an EPUB: the file is not a readable zip archive.")},
    {Failure::Code::EpubProtected, tr("This book is protected by DRM, so only the app it was bought for can open it.")},
    {Failure::Code::EpubWithoutPackage, tr("Not an EPUB: it has no package document.")},
    {Failure::Code::EpubWithoutChapters, tr("This EPUB lists no chapters to read.")},
    {Failure::Code::PdfUnreadable, tr("Not a PDF, or one too damaged to read.")},
    {Failure::Code::PdfLocked, tr("This PDF needs a password to open.")},
    {Failure::Code::PdfWithoutPages, tr("This PDF has no pages.")},
    {Failure::Code::ComicWithoutPictures, tr("This archive holds no pictures to read as a comic.")},
    {Failure::Code::FictionBookEmpty, tr("The file is empty or holds no FictionBook.")},
    {Failure::Code::FictionBookWithoutText, tr("This FictionBook has no text.")},
    {Failure::Code::CatalogTurnsAway, tr("%1 is turning requests away for now; it allows only so many without an account.")},
    {Failure::Code::CatalogUnreachable, tr("%1 can't be reached: %2")},
    {Failure::Code::SourceTooSlow, tr("%1 didn't answer within %n seconds.", "", counted)},
    {Failure::Code::SourceSaid, tr("%1: %2")},
    {Failure::Code::SourceStopped, tr("%1 stopped with an error.")},
    {Failure::Code::ProgramWontStart, tr("%1 can't be started: %2")},
    {Failure::Code::NoDetailsFound, tr("%1 gave no details for this book.")},
    {Failure::Code::SourceNeedsToken, tr("%1 needs your token to be asked: connect it under Reading Sites.")},
    {Failure::Code::NoReadableFile, tr("There is no readable file at %1")},
    {Failure::Code::NotABookFormat, tr("%1 is not a book format Omabook knows.")},
    {Failure::Code::LibraryUnwritable, tr("The library can't be written: %1")},
    {Failure::Code::BookNeedsTitle, tr("A book needs a title.")},
    {Failure::Code::NotOneBook, tr("%1 couldn't be made into one book.")},
    {Failure::Code::NoBooksInside, tr("There are no books inside %1.")},
    {Failure::Code::NoSuchBook, tr("There is no book %1 in the library.")},
    {Failure::Code::InkNotKept, tr("The ink on page %1 could not be kept.")},
    {Failure::Code::NoSuchCollection, tr("There is no collection called %1.")},
    {Failure::Code::NoSuchYear, tr("There is no year %1.")},
    {Failure::Code::GoalUnknownKind, tr("A goal is of books or of hours, not of “%1”.")},
    {Failure::Code::GoalOutOfRange, tr("A goal is a number from 1 to 100000.")},
    {Failure::Code::LibraryBusy, tr("The library is busy; try again.")},
    {Failure::Code::GoalNotKept, tr("The goal couldn't be kept; try again.")},
    {Failure::Code::NoMicrophone, tr("There is no microphone.")},
    {Failure::Code::MicrophoneStopped, tr("The microphone stopped giving sound.")},
    {Failure::Code::MicrophoneUnopened, tr("The microphone could not be opened.")},
    {Failure::Code::NoSpeakers, tr("There is nothing to play sound on.")},
    {Failure::Code::SoundUnplayed, tr("The sound could not be played.")},
    {Failure::Code::SpeakersUnopened, tr("The speakers could not be opened.")},
    {Failure::Code::PacketTooLarge, tr("A packet of %1 bytes is more than a page holds.")},
    {Failure::Code::RecordingUnwritten, tr("The recording could not be written: %1")},
    {Failure::Code::SoundFileUnreadable, tr("%1 could not be read: %2")},
    {Failure::Code::OpusDecoderUnstarted, tr("The Opus decoder did not start: %1")},
    {Failure::Code::NotOpus, tr("%1 is no Opus recording.")},
    {Failure::Code::OpusWithoutSound, tr("%1 ends before its sound begins.")},
    {Failure::Code::OpusEncoderUnstarted, tr("The Opus encoder did not start: %1")},
    {Failure::Code::FolderUnmade, tr("The folder for %1 could not be made.")},
    {Failure::Code::SoundFileUnwritten, tr("%1 could not be written: %2")},
    {Failure::Code::RecordingEnded, tr("The recording has ended.")},
    {Failure::Code::SoundUnencoded, tr("The sound could not be encoded: %1")},
    {Failure::Code::NoInk, tr("There is no ink to recognise.")},
    {Failure::Code::RecordingUnreadable, tr("The recording %1 can't be read.")},
    {Failure::Code::RecordingEmpty, tr("The recording %1 is empty.")},
    {Failure::Code::RecognitionCancelled, tr("The recognition was cancelled.")},
    {Failure::Code::MeaningHosted,
     tr("%1 is someone else's computer, and a book's text goes only to your own servers to be indexed by its meaning, unless you allow "
        "hosted servers for it in the settings.")},
    {Failure::Code::MeaningNoVectors, tr("%1 made no embeddings with %2; is it an embedding model, such as nomic-embed-text?")},
    {Failure::Code::MeaningNotIndexed, tr("The book isn't indexed by its meaning yet.")},
    {Failure::Code::MeaningNoText, tr("The book has no words to index.")},
    {Failure::Code::MeaningCancelled, tr("The indexing was cancelled.")},
    {Failure::Code::NoVisionServer, tr("Handwriting is read by a vision model on your AI server: %1")},
    {Failure::Code::StrokeUnreadable, tr("Stroke %1 can't be read: %2")},
    {Failure::Code::RecognizerUnreachable, tr("%1 can't be reached now (%2).")},
    {Failure::Code::RecognizerBusy, tr("%1 is busy or starting (%2).")},
    {Failure::Code::RecognizerRefusedKey, tr("%1 didn't accept the key; put one in the settings.")},
    {Failure::Code::RecognizerNoSpeech, tr("%1 doesn't transcribe speech.")},
    {Failure::Code::RecognizerNoModel, tr("%1 has no model \"%2\".")},
    {Failure::Code::RecognizerNoVision, tr("The model %1 on %2 can't read pictures; choose one that can for handwriting.")},
    {Failure::Code::RecognizerAnswered, tr("%1 answered: %2")},
    {Failure::Code::RecognizerError, tr("%1 answered with an error (%2).")},
    {Failure::Code::WhisperMissing, tr("whisper.cpp isn't installed on this computer (there is no %1 command).")},
    {Failure::Code::WhisperNoModel,
     tr("whisper.cpp has no model to transcribe with; put a ggml model in ~/.local/share/whisper.cpp/models.")},
    {Failure::Code::WhisperNeedsWav, tr("whisper.cpp reads only WAV recordings, and ffmpeg isn't installed to make one of %1.")},
    {Failure::Code::FfmpegFailed, tr("ffmpeg couldn't read the recording: %1")},
    {Failure::Code::WhisperFailed, tr("whisper.cpp couldn't transcribe the recording: %1")},
    {Failure::Code::WhisperNoTranscript, tr("whisper.cpp wrote no transcript.")},
    {Failure::Code::AiOff, tr("AI is turned off in the settings.")},
    {Failure::Code::AiUnreachable, tr("Can't reach the AI server at %1 (%2).")},
    {Failure::Code::AiTimedOut, tr("The AI server at %1 took too long to answer.")},
    {Failure::Code::AiWantsKey, tr("The AI server at %1 wants a key: put one in the settings.")},
    {Failure::Code::AiRefusedKey, tr("The AI server at %1 didn't accept the key.")},
    {Failure::Code::AnthropicRefusedKey, tr("The Anthropic API didn't accept the key in ANTHROPIC_API_KEY.")},
    {Failure::Code::AiLimited, tr("The AI server at %1 is busy or past its limit for now: %2")},
    {Failure::Code::AiNoModel, tr("The AI server at %1 has no model \"%2\".")},
    {Failure::Code::AiNoModelSaid, tr("The AI server at %1 has no model \"%2\": %3")},
    {Failure::Code::AiAnswered, tr("The AI server at %1 answered: %2")},
    {Failure::Code::AiError, tr("The AI server at %1 answered with an error (%2).")},
    {Failure::Code::AiDeclined, tr("The model declined to answer this.")},
    {Failure::Code::AiListsNoModels, tr("The AI server at %1 lists no models.")},
    {Failure::Code::AiModelsElsewhere,
     tr("Every model the AI server at %1 lists runs elsewhere; pick one in the settings to send your text there.")},
    {Failure::Code::AiNoCommand, tr("No AI command is set.")},
    {Failure::Code::AiPromptUnwritten, tr("The question can't be written to a temporary file.")},
    {Failure::Code::AiStopped, tr("%1 stopped with an error.")},
    {Failure::Code::AiProgramSaid, tr("%1: %2")},
    {Failure::Code::AiModelRefused, tr("%1 didn't take the model \"%2\": %3")},
    {Failure::Code::AgentReachedForTool,
     tr("Antigravity tried to use one of its tools instead of answering, and Omabook doesn't let it. Asking again usually "
        "works; choosing another AI in the settings always does.")},
    {Failure::Code::AgentNoAnswer, tr("Antigravity gave no answer. Run agy once in a terminal to sign in.")},
    {Failure::Code::AiMainModelReplaced,
     // NOLINTNEXTLINE(modernize-raw-string-literal): bin/i18n and the catalogs read plain literals.
     tr("The model \"%1\", chosen for answers and writing, was refused, so \"%2\" answered instead. Check its name in the settings.")},
    {Failure::Code::AiMainModelToDefault,
     tr("The model \"%1\", chosen for answers and writing, was refused, so the default model answered instead. Check its name in "
        "the settings.")},
    {Failure::Code::AiQuickModelReplaced,
     // NOLINTNEXTLINE(modernize-raw-string-literal): bin/i18n and the catalogs read plain literals.
     tr("The model \"%1\", chosen for reading and parsing, was refused, so \"%2\" answered instead. Check its name in the settings.")},
    {Failure::Code::AiQuickModelToDefault,
     tr("The model \"%1\", chosen for reading and parsing, was refused, so the default model answered instead. Check its name in "
        "the settings.")},
    {Failure::Code::NoSuchSpace, tr("This library shares or publishes no space called %1 (omabook spaces lists them).")},
    {Failure::Code::SpaceNameTaken, tr("More than one space goes by %1; name one by its id (omabook spaces lists them).")},
    {Failure::Code::PartNotSwitched, tr("%1 is no part of a space that is switched on and off: %2.")},
    {Failure::Code::UnknownPart, tr("%1 takes %2, and \"%3\" is none of them.")},
    {Failure::Code::OnlyOwnerSwitches, tr("Only the owner of %1 switches what it holds, and you follow it.")},
    {Failure::Code::NoSuchAnnotation, tr("There is no annotation %1, of yours or in an annotation layer.")},
    {Failure::Code::AnnotationWithoutWords, tr("Only a margin note or a highlight with words becomes a fact.")},
    {Failure::Code::NoSuchThing, tr("There is no thing %1 in the book's graph; give a kind to make it.")},
    {Failure::Code::FactNotKept, tr("The words could not be kept as a fact.")},
    {Failure::Code::NotAName, tr("A name to go by in a space is one line of at most %1 characters.")},
    {Failure::Code::NotKeptPrivate, tr("%1 is not a book a plugin keeps private, so it goes into the spaces it is in already.")},
    {Failure::Code::ZipUnchangeable,
     tr("The book's file is a zip Omabook can't change in place: one in parts, a zip64, or a damaged one.")},
    {Failure::Code::EpubPackageUnwritable,
     tr("The details can't be written into this EPUB: its package document is not one Omabook can change.")},
    {Failure::Code::WrittenEpubUnlike,
     tr("The EPUB with the details written into it didn't read back as it should, so the book's file was left as it was.")},
    {Failure::Code::DetailsNeedEpub, tr("Details are written into a book's EPUB, and this book has none.")},
    {Failure::Code::BookFileUnwritable, tr("%1 can't be written. %2")},
    {Failure::Code::NoOriginalKept,
     tr("There is no original of this book's file to put back: its details were never written into it here.")},
});

// The pattern with %1, %2... replaced by the arguments in one pass, so an
// argument that itself holds a "%1" (a file's name can) stays as it is.
[[nodiscard]] QString filled(const QString &pattern, const QStringList &arguments) {
    QString result;
    result.reserve(pattern.size());
    for (qsizetype i = 0; i < pattern.size(); ++i) {
        const qsizetype number = i + 1 < pattern.size() && pattern.at(i) == '%' ? pattern.at(i + 1).digitValue() : -1;
        if (number < 1 || number > arguments.size()) {
            result += pattern.at(i);
            continue;
        }
        result += arguments.at(number - 1);
        ++i;
    }
    return result;
}

} // namespace

QString Failure::text() const {
    // Said words are not Omabook's, so there is nothing to translate.
    const auto *const wording = std::ranges::find(wordings, m_code, &Wording::code);
    if (m_code == Code::Said || wording == wordings.end())
        return m_arguments.join(' ');
    const int count = static_cast<int>(m_count);
    const QString sentence = QCoreApplication::translate("Failure", wording->text, nullptr, count);
    // Without a translator (the command line, the MCP server) Qt hands back the
    // English source, which is a counted sentence's other form ("1 seconds");
    // the English catalog has its one form too.
    if (m_count >= 0 && sentence == QString::fromUtf8(wording->text).replace(u"%n"_s, QString::number(count)))
        return english();
    return filled(sentence, m_arguments);
}

QString Failure::english() const {
    const auto *const wording = std::ranges::find(wordings, m_code, &Wording::code);
    if (m_code == Code::Said || wording == wordings.end())
        return m_arguments.join(' ');
    return filled(Translator::english(wording->text, static_cast<int>(m_count)).replace(u"%n"_s, QString::number(m_count)), m_arguments);
}
