#pragma once

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QtTypes>
#include <cstdint>
#include <utility>

// Why something the layers below the front ends were asked couldn't be done,
// as a code and the words that fill it in (a file's name, a catalog's, the
// system's own reason), never as a sentence (ADR 0005). It is worded where it
// is shown, by text(): in the desktop's language in the window, which installs
// the translator, and in English on the command line and to MCP clients, which
// don't. The sentences are in failure.cpp and the catalogs (src/i18n), so a
// new code is a row there and in the five catalogs.
class Failure {
public:
    enum class Code : std::uint8_t {
        // Words from elsewhere (a plugin, a server, a program), passed on as they were said: {words}.
        Said,
        // Opening a book (formats).
        NoSuchFile,             // {path}
        FileUnreadable,         // {the system's reason}
        FileWithoutText,        // {}
        NoChaptersFound,        // {format, file name}
        UnknownFolder,          // {folder name}
        UnknownFormat,          // {file name}
        FormatOfMissingPlugin,  // {file name, format, plugin's title, plugin's id}
        FormatOfIdlePlugin,     // {file name, format, plugin's title}: installed, and switched off or failing
        OpenedInItsView,        // {file name, format}: its plugin's view opens it, not the reader (#125)
        ReadAfterConverting,    // {format}
        EpubNotAnArchive,       // {}
        EpubProtected,          // {}
        EpubWithoutPackage,     // {}
        EpubWithoutChapters,    // {}
        PdfUnreadable,          // {}
        PdfLocked,              // {}
        PdfWithoutPages,        // {}
        ComicWithoutPictures,   // {}
        FictionBookEmpty,       // {}
        FictionBookWithoutText, // {}
        // Asking a catalog or a plugin's source for a book's details (services).
        CatalogTurnsAway,   // {catalog}
        CatalogUnreachable, // {catalog, the network's reason}
        SourceTooSlow,      // {source}, counted: the seconds it was given
        SourceSaid,         // {source, what it said}
        SourceStopped,      // {source}
        ProgramWontStart,   // {source, program}
        NoDetailsFound,     // {source}
        SourceNeedsToken,   // {reading site}
        // Adding books to the library (library).
        NoReadableFile,    // {path}
        NotABookFormat,    // {file name}
        LibraryUnwritable, // {the database's reason}
        BookNeedsTitle,    // {}
        NotOneBook,        // {folder or archive name}
        NoBooksInside,     // {archive name}
        NoSuchBook,        // {book's number}
        InkNotKept,        // {page number, from 1}
        // One's reading added up, and the year's goal (library, #68).
        NoSuchCollection, // {name}
        NoSuchYear,       // {year}
        GoalUnknownKind,  // {the word given}
        GoalOutOfRange,   // {}
        LibraryBusy,      // {}
        GoalNotKept,      // {}
        // Recording and playing sound (foundation, services).
        NoMicrophone,         // {}
        MicrophoneStopped,    // {}
        MicrophoneUnopened,   // {}
        NoSpeakers,           // {}
        SoundUnplayed,        // {}
        SpeakersUnopened,     // {}
        PacketTooLarge,       // {bytes}
        RecordingUnwritten,   // {the system's reason}
        SoundFileUnreadable,  // {path, the system's reason}
        OpusDecoderUnstarted, // {Opus's reason}
        NotOpus,              // {path}
        OpusWithoutSound,     // {path}
        OpusEncoderUnstarted, // {Opus's reason}
        FolderUnmade,         // {path}
        SoundFileUnwritten,   // {path, the system's reason}
        RecordingEnded,       // {}
        SoundUnencoded,       // {Opus's reason}
        // Recognising handwriting and speech (services); "where" is a recognizer's own words.
        NoInk,                 // {}
        RecordingUnreadable,   // {recording}
        RecordingEmpty,        // {recording}
        RecognitionCancelled,  // {}
        NoVisionServer,        // {the AI server's reason}
        StrokeUnreadable,      // {its number, the reason}
        RecognizerUnreachable, // {where, the network's reason}
        RecognizerBusy,        // {where, HTTP status}
        RecognizerRefusedKey,  // {where}
        RecognizerNoSpeech,    // {where}
        RecognizerNoModel,     // {where, model}
        RecognizerNoVision,    // {model, where}
        RecognizerAnswered,    // {where, what it said}
        RecognizerError,       // {where, HTTP status}
        WhisperMissing,        // {program}
        WhisperNoModel,        // {}
        WhisperNeedsWav,       // {file name}
        FfmpegFailed,          // {what it said}
        WhisperFailed,         // {what it said}
        WhisperNoTranscript,   // {}
        // Spaces and others' annotations (library, operations; #85).
        NoSuchSpace,            // {the name or id asked for}
        SpaceNameTaken,         // {the name asked for}
        PartNotSwitched,        // {the part asked for, the parts that are switched}
        UnknownPart,            // {the option, the parts it takes, the part asked for}
        OnlyOwnerSwitches,      // {the space's name}
        NoSuchAnnotation,       // {the annotation asked for}
        AnnotationWithoutWords, // {}
        NoSuchThing,            // {the thing asked for}
        FactNotKept,            // {}
        NotAName,               // {the longest a name may be, in characters} (#124)
        NotKeptPrivate,         // {book's title}: no plugin keeps its kind private (#127)
        // A book's text found by what it means (services, operations; #72).
        MeaningHosted,     // {where}: someone else's computer, and hosted servers aren't allowed for it
        MeaningNoVectors,  // {where, model}: it answered without the embeddings asked for
        MeaningNotIndexed, // {}: no book asked about is indexed by meaning
        MeaningNoText,     // {}: the book has no words to index
        MeaningCancelled,  // {}
        // A book's details written into its EPUB (formats, library; #74).
        ZipUnchangeable,       // {}
        EpubPackageUnwritable, // {}
        WrittenEpubUnlike,     // {}
        DetailsNeedEpub,       // {}
        BookFileUnwritable,    // {path, the system's reason}
        NoOriginalKept,        // {}
        // E-readers and phones (foundation, services, operations; #71).
        ServerUnstarted,       // {address, the system's reason}
        NoTailnet,             // {}
        SyncNotSetUp,          // {}
        SyncServerUnreachable, // {where, the network's reason}
        SyncServerRefused,     // {where}
        SyncServerAnswered,    // {where, what it said}
        NotAnOpdsFeed,         // {address}
        NoDeviceFolder,        // {folder}
        NothingToSend,         // {book's title}
        NotCopied,             // {where to, the system's reason}
        MailNotSetUp,          // {}
        MailUnreachable,       // {server, the network's reason}
        MailRefused,           // {server, what it said}
        // Asking the AI (services); "where" is a server's address, "name" a harness's or program's.
        // Whether another model is tried decides by these (AiProvider::modelToRetry), never by the words.
        AiOff,               // {}: set to Off in the settings, so nothing is asked of any model or server (#133)
        AiUnreachable,       // {where, the network's reason}
        AiTimedOut,          // {where}
        AiWantsKey,          // {where}
        AiRefusedKey,        // {where}
        AnthropicRefusedKey, // {}
        AiLimited,           // {where, what it said}: busy, or over a rate or a quota
        AiNoModel,           // {where, model}
        AiNoModelSaid,       // {where, model, what it said}
        AiAnswered,          // {where, what it said}
        AiError,             // {where, HTTP status}
        AiDeclined,          // {}
        AiListsNoModels,     // {where}
        AiModelsElsewhere,   // {where}
        AiNoCommand,         // {}
        AiPromptUnwritten,   // {}
        AiStopped,           // {name}
        AiProgramSaid,       // {name, what it said}
        AiModelRefused,      // {name, model, what it said}
        AgentReachedForTool, // {}
        AgentNoAnswer,       // {}
        // Not failures but notes on the way: a refused model, and the one that answered instead.
        AiMainModelReplaced,   // {refused, instead}
        AiMainModelToDefault,  // {refused}
        AiQuickModelReplaced,  // {refused, instead}
        AiQuickModelToDefault, // {refused}
    };

    // An empty one says nothing; it is only there for Qt's containers and signals.
    Failure() = default;
    Failure(Code code, QStringList arguments, qint64 count = -1) : m_code(code), m_arguments(std::move(arguments)), m_count(count) {}
    explicit Failure(Code code) : m_code(code) {}
    [[nodiscard]] static Failure said(const QString &words) { return {Code::Said, {words}}; }

    [[nodiscard]] Code code() const { return m_code; }
    [[nodiscard]] const QStringList &arguments() const { return m_arguments; }
    // The sentence, in the language of the translator installed (English without one).
    [[nodiscard]] QString text() const;
    // The sentence in English whatever translator is installed, for the log.
    [[nodiscard]] QString english() const;

    bool operator==(const Failure &) const = default;

private:
    Code m_code = Code::Said;
    QStringList m_arguments;
    qint64 m_count = -1; // the number a counted sentence's %n stands for
};

Q_DECLARE_METATYPE(Failure)
