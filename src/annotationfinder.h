#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>

class Reader;

// What the reader wrote in the open book, found by its words (#115), for the
// reader's Find: its highlights, margin notes, bookmarks, and the ink of its
// pages whose handwriting was read (BookInkWords), through the word index
// (AnnotationWords). "notes:" in front of the words asks for these alone, as
// it does of the library's search.
class AnnotationFinder : public QObject {
    Q_OBJECT
    // The reader whose open book is looked in (the window's `reader`).
    Q_PROPERTY(QObject *reader READ reader WRITE setReader NOTIFY readerChanged)

public:
    explicit AnnotationFinder(QObject *parent = nullptr);

    [[nodiscard]] QObject *reader() const;
    void setReader(QObject *reader);

    // What holds the words typed, in the order of the book, as the Find
    // lists a place: [{id, kind, section, fraction, title, snippet, quote,
    // lost, note}], `title` saying what it is and where, `snippet` its words,
    // `note` a margin note's number (as MarginNotes::found gives it).
    Q_INVOKABLE QVariantList find(const QString &typed) const;
    // The words of a search for these alone ("notes:harbour" is "harbour"),
    // or nothing when it isn't one.
    Q_INVOKABLE QString onlyNotes(const QString &typed) const;

signals:
    void readerChanged();

private:
    [[nodiscard]] QString whereIs(int section) const;

    QPointer<Reader> m_reader;
};
