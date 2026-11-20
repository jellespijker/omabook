#pragma once

#include <QList>
#include <QNetworkAccessManager>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QUrl>

#include "ai.h"
#include "bookmeta.h"
#include "failure.h"

struct MetaCandidate {
    QString source; // "Open Library", "Google Books", "Amazon.com · Calibre", or the AI's name
    BookMeta meta;
    QUrl coverUrl;
    QString why;    // the AI's reason for what it chose, when it has one

    // From {"title", "authors", ..., "cover_url"}, the shape plugins answer in.
    static MetaCandidate fromJson(const QJsonObject &object, const QString &source);
    QJsonObject toJson() const;
};

// What taking a candidate would do to a book: the fields it would fill in,
// and the ones it would change, with both values. Fields are named by their
// keys: title, authors, series, publisher, published, language, isbn,
// description, tags, cover.
struct MetaDiff {
    struct Change {
        QString field, from, to;
    };
    QStringList adds;
    QList<Change> changes;
};
MetaDiff whatItBrings(const BookMeta &book, bool bookHasCover, const MetaCandidate &candidate);

// Somewhere a book's details can be looked up. Each lookup answers once, with
// candidates or with an error; the window shows them side by side and the
// user picks. A new catalog is one subclass.
class MetadataSource : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    virtual QString name() const = 0;
    // The fields this source can bring, by their keys.
    virtual QStringList fields() const = 0;
    // Looks the book up by its ISBN when it has one, else by title and author.
    virtual void search(const BookMeta &book) = 0;

signals:
    void found(const QList<MetaCandidate> &candidates);
    void failed(const Failure &failure);
};

// A catalog asked over HTTP: the subclass says which address to ask and how
// to read the answer.
class WebMetadataSource : public MetadataSource {
    Q_OBJECT

public:
    explicit WebMetadataSource(QNetworkAccessManager *network, QObject *parent = nullptr)
        : MetadataSource(parent), m_network(network) {}

    void search(const BookMeta &book) override;

protected:
    virtual QUrl searchUrl(const BookMeta &book) const = 0;
    virtual QList<MetaCandidate> parse(const QByteArray &answer) const = 0;

private:
    QNetworkAccessManager *m_network;
};

class OpenLibrarySource : public WebMetadataSource {
    Q_OBJECT

public:
    using WebMetadataSource::WebMetadataSource;
    QString name() const override { return "Open Library"; }
    QStringList fields() const override { return {"title", "authors", "publisher", "published", "isbn", "tags", "cover"}; }
    static QList<MetaCandidate> parseAnswer(const QByteArray &answer);

    // Public so the addresses can be tested without the network.
    QUrl searchUrl(const BookMeta &book) const override;

protected:
    QList<MetaCandidate> parse(const QByteArray &answer) const override { return parseAnswer(answer); }
};

class GoogleBooksSource : public WebMetadataSource {
    Q_OBJECT

public:
    using WebMetadataSource::WebMetadataSource;
    QString name() const override { return "Google Books"; }
    QStringList fields() const override {
        return {"title", "authors", "publisher", "published", "language", "isbn", "description", "tags", "cover"};
    }
    static QList<MetaCandidate> parseAnswer(const QByteArray &answer);

    // Public so the addresses can be tested without the network.
    QUrl searchUrl(const BookMeta &book) const override;

protected:
    QList<MetaCandidate> parse(const QByteArray &answer) const override { return parseAnswer(answer); }
};

// Apple's bookstore: good covers and descriptions for books that are sold.
class AppleBooksSource : public WebMetadataSource {
    Q_OBJECT

public:
    using WebMetadataSource::WebMetadataSource;
    QString name() const override { return "Apple Books"; }
    QStringList fields() const override { return {"title", "authors", "published", "description", "tags", "cover"}; }
    static QList<MetaCandidate> parseAnswer(const QByteArray &answer);

    // Public so the addresses can be tested without the network.
    QUrl searchUrl(const BookMeta &book) const override;

protected:
    QList<MetaCandidate> parse(const QByteArray &answer) const override { return parseAnswer(answer); }
};

// Crossref: the registry of scholarly publishing, for textbooks, monographs
// and anything with a DOI, which the bookshops know little about.
class CrossrefSource : public WebMetadataSource {
    Q_OBJECT

public:
    using WebMetadataSource::WebMetadataSource;
    QString name() const override { return "Crossref"; }
    QStringList fields() const override { return {"title", "authors", "publisher", "published", "isbn", "description", "tags"}; }
    static QList<MetaCandidate> parseAnswer(const QByteArray &answer);

    // Public so the addresses can be tested without the network.
    QUrl searchUrl(const BookMeta &book) const override;

protected:
    QList<MetaCandidate> parse(const QByteArray &answer) const override { return parseAnswer(answer); }
};

// A source that is a program: it is handed the book as JSON on standard
// input and answers {"candidates": [...]} on standard output. Omabook's own
// plugins are these, and so is the bridge that runs Calibre's.
class ProcessMetadataSource : public MetadataSource {
    Q_OBJECT

public:
    // `program` and `arguments` are run in `folder` (the current one when empty).
    ProcessMetadataSource(const QString &name, const QStringList &fields, const QString &program,
                          const QStringList &arguments, const QString &folder, QObject *parent = nullptr);

    QString name() const override { return m_name; }
    QStringList fields() const override { return m_fields; }
    void search(const BookMeta &book) override;

protected:
    // What the program is sent, and how its output is read.
    virtual QByteArray request(const BookMeta &book) const;
    virtual QList<MetaCandidate> read(const QByteArray &output, QString *problem) const;

private:
    QString m_name, m_program, m_folder;
    QStringList m_fields, m_arguments;
    QProcess m_process;
    QTimer m_timeout;
    bool m_timedOut = false;
};

// One of Calibre's own metadata source plugins, run inside Calibre through
// calibre_bridge.py: the built-in ones and any the user installed there.
class CalibreMetadataSource : public ProcessMetadataSource {
    Q_OBJECT

public:
    CalibreMetadataSource(const QString &plugin, const QStringList &fields, const QString &calibreDebug,
                          const QString &bridge, QObject *parent = nullptr);

protected:
    QByteArray request(const BookMeta &book) const override;
    QList<MetaCandidate> read(const QByteArray &output, QString *problem) const override;

private:
    QString m_plugin;
};

// The model, as a source. With nothing else to go on it reads the book's own
// opening pages and says what the book is, which works for a file no catalog
// knows. Given what the catalogs found, it also judges them: which result is
// this book, and which of their fields to trust.
class AiMetadataSource : public MetadataSource {
    Q_OBJECT

public:
    // `provider` must outlive this; `fileName` and `opening` describe the book.
    AiMetadataSource(AiProvider *provider, const QString &fileName, const QString &opening, QObject *parent = nullptr);

    QString name() const override { return m_provider->name(); }
    QStringList fields() const override {
        return {"title", "authors", "series", "publisher", "published", "language", "isbn", "description", "tags", "cover"};
    }
    // What the other sources found, for the model to weigh.
    void setFound(const QList<MetaCandidate> &candidates) { m_found = candidates; }
    void search(const BookMeta &book) override;

private:
    AiProvider *m_provider;
    QString m_fileName, m_opening, m_answer;
    QList<MetaCandidate> m_found;
};

// Fetches a candidate's cover picture.
void fetchCover(QNetworkAccessManager *network, const QUrl &url, QObject *context,
                std::function<void(const QImage &)> done);
