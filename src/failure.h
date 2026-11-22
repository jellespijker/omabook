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
        VoxtypeMissing,        // {program}
        VoxtypeNeedsWav,       // {file name}
        VoxtypeFailed,         // {what it said}
        // Dictating into a field (services, #148).
        DictationBusy,          // {}: another field is listened for
        DictationNothingHeard,  // {}
        VoxtypeDictationFailed, // {what it said}
        // Writing recordings out (formats, services; #151).
        NotTranscribed,        // {file name}: a recording in the library, not written out yet
        TranscriptUnwritten,   // {path, the system's reason}
        RecordingNeedsFfmpeg,  // {file name}: neither a WAV nor an Opus recording, and no ffmpeg to read it
        TranscriptionStopped,  // {}: stopped; what was written out is kept
        // Reading a book aloud (services; #73); "name" is the engine's, "where" a server's address.
        NoVoice,            // {}: neither Piper with a voice nor speech-dispatcher is installed
        PiperMissing,       // {}
        PiperNoVoice,       // {}
        DispatcherMissing,  // {}
        VoiceAiOff,         // {}: the AI is off, so no server is asked
        VoiceServerHosted,  // {where}: someone else's computer, and hosted servers aren't allowed for it
        VoiceServerFailed,  // {where, HTTP status or the network's reason}
        VoiceStopped,       // {name, what it said}
        VoiceUnplayed,      // {the player's reason}
        VoiceOpenAiNoKey,   // {}: OpenAI's voices, and no key for it (#147)
        VoiceModelRetired,  // {where, model}: the server retired the model, and there is none to fall back to (#162)
        VoiceKeyRefused,    // {where}: the server didn't accept the key (OpenAI's Realtime API, #169)
        VoiceNoSound,       // {where}: it answered a sentence to read aloud without its sound (#169)
        NothingToReadAloud, // {}: pages of pictures
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
        // The port taken when Omabook would serve on it (#177): by another program, or by an Omabook serving already.
        PortTaken,           // {address, port}
        PortTakenBy,         // {address, port, program}
        ServedByWindow,      // {address}
        ServedByService,     // {address}
        ServedByCommand,     // {address}
        ServiceUnwritable,   // {path, the system's reason}
        ServiceRefused,      // {what systemctl said}
        NoSystemd,           // {}
        ServiceNotAnswering, // {address}
        // The hub (#179): the libraries it keeps, alone, and the requests it takes over its socket.
        LibraryKeptByHub,    // {folder, the daemon's process}
        HubNotRunning,       // {socket}
        HubRunning,          // {socket}
        HubCantListen,       // {socket, the system's reason}
        HubNoLibraries,      // {}: no folder chosen for its libraries yet
        HubNoSuchLibrary,    // {name}
        HubLibraryUnnamed,   // {}: it keeps more than one, and the request named none
        HubUnknownRequest,   // {request}
        HubBadRequest,       // {}
        HubSecretOpen,       // {file}: others may read it
        HubSecretUnreadable, // {file, the system's reason}
        // Asking the AI (services); "where" is a server's address, "name" a harness's or program's.
        // Whether another model is tried decides by these (AiProvider::modelToRetry), never by the words.
        AiOff,               // {}: set to Off in the settings, so nothing is asked of any model or server (#133)
        AiHostedNotAllowed,      // {name}: someone else's computer, and hosted isn't allowed for answers (ADR 0008)
        AiQuickHostedNotAllowed, // {name}: the same for quick work
        HostedNotChosen,         // {where}: someone else's computer, and the plugin asking didn't ask for a hosted one
        AiUnreachable,       // {where, the network's reason}
        AiTimedOut,          // {where}
        AiQuickWaits,        // {where}: quick work waits for its server, away, rather than go to answers' fallback (#174)
        NoFallback,          // {}: no fallback to try, or none that may be used (#174)
        AiWantsKey,          // {where}
        AiRefusedKey,        // {where}
        AnthropicRefusedKey, // {}
        GeminiNoKey,         // {}: no key for the Gemini API in the keyring (#145)
        GeminiRefusedKey,    // {}
        GeminiNoSound,       // {}: it answered a sentence to read aloud without its sound
        RecordingTooLarge,   // {recording, the most it takes in MB}: too long to be sent at once
        KeyNotKept,          // {the keyring's reason}
        KeyNotForgotten,     // {}
        AiLimited,           // {where, what it said}: busy, or over a rate or a quota
        AiNoModel,           // {where, model}
        AiNoModelSaid,       // {where, model, what it said}
        AiAnswered,          // {where, what it said}
        AiError,             // {where, HTTP status}
        AiDeclined,          // {}
        AiListsNoModels,     // {where}
        AiModelsElsewhere,   // {where}
        ModelNotPulled,      // {model, where, what it said}: Ollama didn't download it (#172)
        ModelSearchUnanswered, // {site, the network's reason or its HTTP status}: a search for models (#175)
        ModelHosted,         // {model}: it runs on Ollama's computers, and hosted isn't allowed for the capability
        NoServerToInstallOn, // {}: no Ollama server for the capability to install a model onto
        ModelNotDownloaded,  // {model, why}: a speech model's file didn't come down
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
        // The chronology's timelines that branch (library, #171).
        NoThingNumbered,       // {its number}
        NotOfKind,             // {its name, its kind, the kind asked for}
        TimelineNoBranch,      // {name}: a timeline of that name is there, and is no branch
        BranchOffItself,       // {the branch, the timeline it would branch off}
        BranchNotKept,         // {}
        BranchesOffNothing,    // {timeline}
        BranchWordNotKept,     // {branch}
        BranchMadeByBranching, // {}: a branch is not added as a kind of timeline
        AiAnsweredInstead,     // {who answered, the first, away}: the fallback answered (#174)
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
