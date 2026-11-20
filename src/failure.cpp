#include "failure.h"

#include <QCoreApplication>
#include <algorithm>
#include <array>

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
    {Failure::Code::NoInk, tr("There is no ink to recognise.")},
    {Failure::Code::RecordingUnreadable, tr("The recording %1 can't be read.")},
    {Failure::Code::RecordingEmpty, tr("The recording %1 is empty.")},
    {Failure::Code::RecognitionCancelled, tr("The recognition was cancelled.")},
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
    return filled(QCoreApplication::translate("Failure", wording->text, nullptr, static_cast<int>(m_count)), m_arguments);
}
