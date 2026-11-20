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

    bool operator==(const Failure &) const = default;

private:
    Code m_code = Code::Said;
    QStringList m_arguments;
    qint64 m_count = -1; // the number a counted sentence's %n stands for
};

Q_DECLARE_METATYPE(Failure)
