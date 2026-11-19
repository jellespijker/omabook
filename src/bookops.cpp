#include "bookops.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <algorithm>
#include <cstdio>

#include "calibreplugin.h"
#include "converter.h"
#include "formats.h"
#include "plugins.h"
#include "prompts.h"
#include "scriptplugins.h"
#include "settings.h"

QString BookOps::readableFile(const Library &library, qint64 id, QString *error) {
    const auto book = library.book(id);
    if (!book) {
        *error = QString("There is no book %1 in the library.").arg(id);
        return {};
    }
    for (const QString &format : book->formats)
        if (Formats::readable(format))
            return library.formatPath(id, format);
    *error = book->formats.isEmpty()
                 ? QString("Book %1 has no file in the library, so there is nothing to read.").arg(id)
                 : QString("Book %1 has no format Omabook reads itself; run: omabook convert %1").arg(id);
    return {};
}

std::unique_ptr<Document> BookOps::open(const Library *library, const QString &numberOrFile, QString *error) {
    bool number = false;
    const qint64 id = numberOrFile.toLongLong(&number);
    QString file;
    if (number) {
        if (!library) {
            *error = QString("There is no book %1 in the library.").arg(id);
            return nullptr;
        }
        file = readableFile(*library, id, error);
        if (file.isEmpty())
            return nullptr;
    } else {
        file = QFileInfo(numberOrFile).absoluteFilePath();
        if (!QFileInfo::exists(file)) { // a file, or a folder that is one book
            *error = "There is no book or file called " + numberOrFile + ".";
            return nullptr;
        }
    }
    return Formats::open(file, error);
}

QJsonArray BookOps::find(const Document &document, const QString &words, int limit, bool *more) {
    QJsonArray matches;
    if (more)
        *more = false;
    if (words.isEmpty())
        return matches;
    for (int section = 0; section < document.count(); ++section) {
        const QString text = document.text(section).simplified();
        for (qsizetype at = text.indexOf(words, 0, Qt::CaseInsensitive); at >= 0;
             at = text.indexOf(words, at + words.size(), Qt::CaseInsensitive)) {
            if (limit >= 0 && matches.size() >= limit) {
                if (more)
                    *more = true;
                return matches;
            }
            matches << QJsonObject{{"section", section},
                                   {"snippet", text.mid(qMax<qsizetype>(0, at - 60), words.size() + 120)}};
        }
    }
    return matches;
}

bool BookOps::ask(const AiRequest &request, QString *answer, QString *error, const std::function<void(const QString &)> &piece,
                  QString *note) {
    const Settings settings;
    std::unique_ptr<AiProvider> provider = makeAiProvider(settings, error);
    if (!provider)
        return false;
    AiCall call(std::move(provider));
    QEventLoop loop;
    bool ok = false;
    QObject::connect(&call, &AiCall::delta, &loop, [&piece](const QString &text) {
        if (piece)
            piece(text);
    });
    QObject::connect(&call, &AiCall::done, &loop, [answer, &ok, &loop](const QString &text) {
        *answer = text;
        ok = true;
        loop.quit();
    });
    QObject::connect(&call, &AiCall::failed, &loop, [error, &loop](const QString &problem) {
        *error = problem;
        loop.quit();
    });
    QObject::connect(&call, &AiCall::noted, &loop, [note](const QString &said) {
        // Never lost: with nobody to hand it to, it goes to standard error.
        if (note)
            *note = said;
        else
            fprintf(stderr, "%s\n", qPrintable(said));
    });
    call.ask(request);
    loop.exec();
    return ok;
}

AiRequest BookOps::bookQuestion(const Document &document, const QString &question, int lastSection, bool *trimmed,
                                const QString &known) {
    const int last = lastSection < 0 ? document.count() - 1 : qBound(0, lastSection, document.count() - 1);
    QString words;
    for (int section = 0; section <= last; ++section)
        words += document.text(section) + "\n\n";
    const Prompts::Excerpt excerpt = Prompts::fit(words);
    if (trimmed)
        *trimmed = excerpt.trimmed;
    AiRequest request;
    request.system = Prompts::companion(document.meta());
    request.messages.append({"user", Prompts::question(excerpt.text, {}, question, known)});
    return request;
}

QList<Curation::BookNotes> BookOps::booksWithNotes(const Library &library) {
    QList<Curation::BookNotes> books;
    for (const Book &book : library.books())
        books.append({book, library.notes(book.id, "recap").text});
    return books;
}

BookOps::LookUp BookOps::lookUp(const Library &library, const Book &book, bool ai) {
    LookUp result;
    Settings settings;
    const Plugins plugins(&settings);
    QNetworkAccessManager network;
    // Asks a set of sources and waits for them all.
    auto askAll = [&](const QList<MetadataSource *> &sources) {
        if (sources.isEmpty())
            return;
        int waiting = sources.size();
        // In the order the sources are listed, not the order they answer in,
        // so the same look-up numbers its candidates the same way each time.
        QList<QList<MetaCandidate>> answers(sources.size());
        QEventLoop loop;
        for (qsizetype at = 0; at < sources.size(); ++at) {
            MetadataSource *source = sources.at(at);
            QObject::connect(source, &MetadataSource::found, &loop, [&answers, &waiting, &loop, at](const QList<MetaCandidate> &found) {
                answers[at] = found;
                if (--waiting == 0)
                    loop.quit();
            });
            QObject::connect(source, &MetadataSource::failed, &loop, [&result, &waiting, &loop](const QString &problem) {
                result.problems << problem;
                if (--waiting == 0)
                    loop.quit();
            });
            source->search(book.meta);
        }
        loop.exec();
        QList<MetaCandidate> found;
        for (const QList<MetaCandidate> &answer : std::as_const(answers))
            found += answer;
        // The AI's judgement is the recommendation, so it leads.
        result.candidates = qobject_cast<AiMetadataSource *>(sources.first()) ? found + result.candidates : result.candidates + found;
    };
    // The sources script plugins add are asked with the others.
    CommandScripts scripts(&settings, library.root());
    askAll(plugins.sources(plugins.list(), &network, &network) + scripts.plugins().metadataSources(&network));
    if (!ai)
        return result;

    const std::unique_ptr<AiProvider> provider = makeAiProvider(settings, &result.error);
    if (!provider)
        return result;
    QString opening;
    for (const QString &format : book.formats) {
        if (!Formats::readable(format))
            continue;
        if (const auto document = Formats::open(library.formatPath(book.id, format)))
            for (int section = 0; section < document->count() && opening.size() < 6000; ++section)
                opening += document->text(section) + "\n\n";
        break;
    }
    // Owned by the network manager, like the other sources.
    auto *judge = new AiMetadataSource(provider.get(), QFileInfo(library.formatPath(book.id)).fileName(), opening.left(6000),
                                       &network);
    judge->setFound(result.candidates);
    askAll({judge});
    return result;
}

QJsonObject BookOps::candidateJson(const Book &book, const MetaCandidate &candidate) {
    const MetaDiff diff = whatItBrings(book.meta, book.hasCover, candidate);
    QJsonArray changes;
    for (const MetaDiff::Change &change : diff.changes)
        changes << QJsonObject{{"field", change.field}, {"from", change.from}, {"to", change.to}};
    QJsonObject object = candidate.toJson();
    object.insert("adds", QJsonArray::fromStringList(diff.adds));
    object.insert("changes", changes);
    return object;
}

QString BookOps::candidateId(const MetaCandidate &candidate) {
    // What it would give the book; the AI's "why" is how it was chosen.
    QJsonObject details = candidate.toJson();
    details.remove("why");
    const QByteArray hash = QCryptographicHash::hash(QJsonDocument(details).toJson(QJsonDocument::Compact), QCryptographicHash::Sha1);
    QString name;
    for (const QChar c : candidate.source.normalized(QString::NormalizationForm_KD).toLower())
        if (c.isLetterOrNumber() && c.unicode() < 128)
            name += c;
        else if (!name.isEmpty() && !name.endsWith('-'))
            name += '-';
    while (name.endsWith('-'))
        name.chop(1);
    return (name.isEmpty() ? QString("candidate") : name.left(24)) + "-" + QString::fromLatin1(hash.toHex().left(8));
}

namespace {

// Where the last listing of a book's candidates is kept, per library.
QString listingFile(const Library &library, qint64 bookId) {
    const QByteArray which = QCryptographicHash::hash(library.root().toUtf8(), QCryptographicHash::Sha1).toHex().left(12);
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
           + QString("/omabook/lookups/%1-%2.json").arg(QString::fromLatin1(which)).arg(bookId);
}

} // namespace

void BookOps::rememberListing(const Library &library, qint64 bookId, bool ai, const QList<MetaCandidate> &candidates) {
    QJsonArray list;
    for (const MetaCandidate &candidate : candidates) {
        QJsonObject object = candidate.toJson();
        object.insert("candidate_id", candidateId(candidate));
        list << object;
    }
    const QString file = listingFile(library, bookId);
    QDir().mkpath(QFileInfo(file).absolutePath());
    QSaveFile out(file);
    if (out.open(QIODevice::WriteOnly)) {
        out.write(QJsonDocument(QJsonObject{{"book", bookId}, {"ai", ai}, {"at", QDateTime::currentSecsSinceEpoch()},
                                            {"candidates", list}})
                      .toJson(QJsonDocument::Compact));
        out.commit();
    }
}

std::optional<BookOps::Listing> BookOps::lastListing(const Library &library, qint64 bookId) {
    QFile in(listingFile(library, bookId));
    if (!in.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QJsonObject object = QJsonDocument::fromJson(in.readAll()).object();
    const qint64 age = QDateTime::currentSecsSinceEpoch() - object.value("at").toInteger();
    if (object.value("book").toInteger() != bookId || age < 0 || age > listingLife)
        return std::nullopt;
    Listing listing;
    listing.ai = object.value("ai").toBool();
    for (const QJsonValue &value : object.value("candidates").toArray()) {
        listing.candidates << MetaCandidate::fromJson(value.toObject(), value["source"].toString());
        listing.ids << value["candidate_id"].toString();
    }
    return listing;
}

void BookOps::forgetListing(const Library &library, qint64 bookId) { QFile::remove(listingFile(library, bookId)); }

bool BookOps::applyCandidate(Library &library, qint64 id, const MetaCandidate &candidate, QString *error) {
    const auto book = library.book(id);
    if (!book) {
        *error = QString("There is no book %1 in the library.").arg(id);
        return false;
    }
    BookMeta meta = book->meta;
    meta.apply(candidate.meta);
    if (candidate.coverUrl.isValid()) {
        QNetworkAccessManager network;
        QEventLoop wait;
        fetchCover(&network, candidate.coverUrl, &wait, [&](const QImage &cover) {
            if (cover.width() > 50)
                meta.cover = cover;
            wait.quit();
        });
        wait.exec();
    }
    return library.setMeta(id, meta, error);
}

std::expected<void, QString> BookOps::setIdentifiers(Library &library, qint64 id, const QList<QPair<QString, QString>> &identifiers,
                                                     std::optional<qint64> at) {
    if (!library.book(id))
        return std::unexpected(QString("There is no book %1 in the library.").arg(id));
    static const QRegularExpression unfit("[\\s=]");
    for (const auto &[scheme, value] : identifiers) {
        if (scheme.trimmed().isEmpty() || unfit.match(scheme.trimmed()).hasMatch())
            return std::unexpected(
                QString("\"%1\" is not a scheme: name one such as goodreads, isbn or calibre, without spaces.").arg(scheme));
        if (scheme.trimmed().compare("generated", Qt::CaseInsensitive) == 0)
            return std::unexpected("\"generated\" says which plugin made a book, and only that plugin sets it, when it makes the book.");
    }
    for (const auto &[scheme, value] : identifiers)
        if (!library.setIdentifier(id, scheme.trimmed(), value.trimmed(), at))
            return std::unexpected("The library can't be written.");
    return {};
}

QString BookOps::convert(Library &library, qint64 id, const QString &to, const QString &from, QString *error) {
    const QString source = library.formatPath(id, from);
    if (source.isEmpty()) {
        *error = "There is no such book or format in the library.";
        return {};
    }
    const QString format = to.isEmpty() ? QString("epub") : to.toLower();
    if (Formats::extension(source) == format) {
        *error = "The book already is " + Formats::name(format) + ".";
        return {};
    }
    const QTemporaryDir scratch;
    const QString output = scratch.filePath("converted." + format);
    // Calibre's converter, unless the Calibre plugin is switched off.
    Settings settings;
    const CalibrePlugin calibre(&settings);
    Converter converter(calibre.converter());
    converter.setProgram(calibre.converter(), calibre.converterProblem());
    QEventLoop loop;
    QObject::connect(&converter, &Converter::finished, &loop, [error, &loop](const QString &, const QString &problem) {
        *error = problem;
        loop.quit();
    });
    QMetaObject::invokeMethod(&converter, [&converter, &source, &output] { converter.convert(source, output); }, Qt::QueuedConnection);
    loop.exec();
    if (!error->isEmpty() || !library.addFormat(id, output, error))
        return {};
    return library.formatPath(id, format);
}

QList<Proposal> BookOps::tidy(const QList<Book> &books, const QList<Proposal> &series) {
    QList<Proposal> proposals = Curation::duplicates(books) + Curation::authorSpellings(books) + series;
    // Merges last: they take books away that other proposals may name.
    std::ranges::stable_sort(proposals,
                     [](const Proposal &a, const Proposal &b) { return a.kind != "duplicate" && b.kind == "duplicate"; });
    return proposals;
}

QJsonObject BookOps::proposalJson(const Book &book, const Proposal &proposal) {
    QJsonObject object{{"kind", proposal.kind}, {"id", proposal.bookId}, {"title", book.meta.title}};
    if (proposal.kind == "duplicate") {
        object.insert("folds_in", proposal.otherId);
        // Unsure: the titles only agree once a subtitle is left off.
        object.insert("confidence", proposal.unsure ? "low" : "high");
    } else if (proposal.kind == "author") {
        object.insert("authors", QJsonArray::fromStringList(proposal.meta.authors));
    } else {
        object.insert("series", proposal.meta.series);
        object.insert("series_index", proposal.meta.seriesIndex);
    }
    if (!proposal.why.isEmpty())
        object.insert("why", proposal.why);
    return object;
}

Proposal BookOps::proposalFromJson(const QJsonObject &object) {
    Proposal proposal;
    proposal.kind = object.value("kind").toString();
    proposal.bookId = object.value("id").toInteger();
    proposal.otherId = object.value("folds_in").toInteger();
    for (const QJsonValue &author : object.value("authors").toArray())
        proposal.meta.authors << author.toString();
    proposal.meta.series = object.value("series").toString();
    proposal.meta.seriesIndex = object.value("series_index").toDouble();
    return proposal;
}
