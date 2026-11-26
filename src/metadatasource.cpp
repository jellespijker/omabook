#include "metadatasource.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>

#include "htmlclean.h"
#include "prompts.h"

namespace {

QNetworkRequest request(const QUrl &url) {
    QNetworkRequest http(url);
    // Catalogs ask clients to say who they are; the app's name says enough.
    http.setHeader(QNetworkRequest::UserAgentHeader, "Omabook/" OMABOOK_VERSION);
    http.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    http.setTransferTimeout(15000);
    return http;
}

// A search address with every value percent-encoded once. QUrlQuery keeps
// some characters (a space, a quote) as they are, which servers refuse.
QUrl searchAddress(const QString &base, const QList<QPair<QString, QString>> &items) {
    QStringList parts;
    for (const auto &[key, value] : items)
        parts << key + "=" + QString::fromLatin1(QUrl::toPercentEncoding(value));
    QUrl url(base);
    url.setQuery(parts.join('&'), QUrl::StrictMode);
    return url;
}

QStringList strings(const QJsonValue &value, int most = 100) {
    QStringList list;
    for (const QJsonValue &item : value.toArray())
        if (list.size() < most && !item.toString().isEmpty())
            list << item.toString();
    return list;
}

} // namespace

MetaCandidate MetaCandidate::fromJson(const QJsonObject &object, const QString &source) {
    MetaCandidate candidate;
    candidate.source = source;
    candidate.meta = BookMeta::fromJson(object);
    if (object.value("published").isDouble())
        candidate.meta.published = QString::number(object.value("published").toInt());
    if (candidate.meta.description.contains('<'))
        candidate.meta.description = htmlToText(candidate.meta.description);
    candidate.coverUrl = QUrl(object.value("cover_url").toString());
    candidate.why = object.value("why").toString().simplified();
    return candidate;
}

QJsonObject MetaCandidate::toJson() const {
    QJsonObject object = meta.toJson();
    object.insert("source", source);
    object.insert("cover_url", coverUrl.toString());
    if (!why.isEmpty())
        object.insert("why", why);
    return object;
}

MetaDiff whatItBrings(const BookMeta &book, bool bookHasCover, const MetaCandidate &candidate) {
    MetaDiff diff;
    auto compare = [&](const QString &field, const QString &mine, const QString &theirs) {
        if (theirs.isEmpty() || mine.compare(theirs, Qt::CaseSensitive) == 0)
            return;
        if (mine.isEmpty())
            diff.adds << field;
        else
            diff.changes.append({field, mine, theirs});
    };
    const BookMeta &found = candidate.meta;
    compare("title", book.title, found.title);
    compare("authors", book.authorText(), found.authorText());
    compare("series", book.series, found.series);
    compare("publisher", book.publisher, found.publisher);
    compare("published", book.published, found.published);
    compare("language", book.language, found.language);
    compare("isbn", book.isbn, found.isbn);
    compare("description", book.description, found.description);
    compare("tags", book.tags.join(", "), found.tags.join(", "));
    if (candidate.coverUrl.isValid()) {
        if (bookHasCover)
            diff.changes.append(MetaDiff::Change{.field = "cover", .from = {}, .to = {}});
        else
            diff.adds << "cover";
    }
    return diff;
}

void WebMetadataSource::search(const BookMeta &book) {
    QNetworkReply *reply = m_network->get(request(searchUrl(book)));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 429)
            emit failed(Failure(Failure::Code::CatalogTurnsAway, {name()}));
        else if (reply->error() != QNetworkReply::NoError)
            emit failed(Failure(Failure::Code::CatalogUnreachable, {name(), reply->errorString()}));
        else
            emit found(parse(reply->readAll()));
    });
}

QUrl OpenLibrarySource::searchUrl(const BookMeta &book) const {
    QList<QPair<QString, QString>> items;
    if (!book.isbn.isEmpty()) {
        items.emplaceBack("isbn", book.isbn);
    } else {
        items.emplaceBack("title", book.title);
        if (!book.authors.isEmpty())
            items.emplaceBack("author", book.authors.first());
    }
    items.emplaceBack("limit", "5");
    items.emplaceBack("fields", "title,author_name,first_publish_year,publisher,isbn,language,subject,cover_i");
    return searchAddress("https://openlibrary.org/search.json", items);
}

QList<MetaCandidate> OpenLibrarySource::parseAnswer(const QByteArray &answer) {
    QList<MetaCandidate> candidates;
    for (const QJsonValue &value : QJsonDocument::fromJson(answer).object().value("docs").toArray()) {
        const QJsonObject doc = value.toObject();
        MetaCandidate candidate;
        candidate.source = "Open Library";
        candidate.meta.title = doc.value("title").toString();
        candidate.meta.authors = strings(doc.value("author_name"));
        if (doc.value("first_publish_year").isDouble())
            candidate.meta.published = QString::number(doc.value("first_publish_year").toInt());
        candidate.meta.publisher = strings(doc.value("publisher")).value(0);
        // A work lists the ISBN of every edition; a 13-digit one is the modern form.
        const QStringList isbns = strings(doc.value("isbn"));
        for (const QString &isbn : isbns)
            if (candidate.meta.isbn.isEmpty() && isbn.size() == 13)
                candidate.meta.isbn = isbn;
        candidate.meta.tags = strings(doc.value("subject"), 5);
        if (doc.value("cover_i").isDouble())
            candidate.coverUrl = QUrl(QString("https://covers.openlibrary.org/b/id/%1-L.jpg").arg(doc.value("cover_i").toInteger()));
        if (!candidate.meta.title.isEmpty())
            candidates << candidate;
    }
    return candidates;
}

QUrl GoogleBooksSource::searchUrl(const BookMeta &book) const {
    QString terms;
    if (!book.isbn.isEmpty()) {
        terms = "isbn:" + book.isbn;
    } else {
        terms = "intitle:\"" + book.title + "\"";
        if (!book.authors.isEmpty())
            terms += " inauthor:\"" + book.authors.first() + "\"";
    }
    return searchAddress("https://www.googleapis.com/books/v1/volumes", {{"q", terms}, {"maxResults", "5"}});
}

QList<MetaCandidate> GoogleBooksSource::parseAnswer(const QByteArray &answer) {
    QList<MetaCandidate> candidates;
    for (const QJsonValue &value : QJsonDocument::fromJson(answer).object().value("items").toArray()) {
        const QJsonObject info = value.toObject().value("volumeInfo").toObject();
        MetaCandidate candidate;
        candidate.source = "Google Books";
        candidate.meta.title = info.value("title").toString();
        if (!info.value("subtitle").toString().isEmpty())
            candidate.meta.title += ": " + info.value("subtitle").toString();
        candidate.meta.authors = strings(info.value("authors"));
        candidate.meta.publisher = info.value("publisher").toString();
        candidate.meta.published = info.value("publishedDate").toString();
        candidate.meta.language = info.value("language").toString();
        candidate.meta.tags = strings(info.value("categories"), 5);
        const QString description = info.value("description").toString();
        candidate.meta.description = description.contains('<') ? htmlToText(description) : description;
        for (const QJsonValue &identifier : info.value("industryIdentifiers").toArray())
            if (identifier.toObject().value("type").toString() == "ISBN_13")
                candidate.meta.isbn = identifier.toObject().value("identifier").toString();
        QString cover = info.value("imageLinks").toObject().value("thumbnail").toString();
        if (!cover.isEmpty())
            candidate.coverUrl = QUrl(cover.replace("http://", "https://").replace("&edge=curl", ""));
        if (!candidate.meta.title.isEmpty())
            candidates << candidate;
    }
    return candidates;
}

QUrl AppleBooksSource::searchUrl(const BookMeta &book) const {
    return searchAddress("https://itunes.apple.com/search",
                         {{"media", "ebook"}, {"limit", "5"}, {"term", (book.title + " " + book.authors.value(0)).simplified()}});
}

QList<MetaCandidate> AppleBooksSource::parseAnswer(const QByteArray &answer) {
    QList<MetaCandidate> candidates;
    for (const QJsonValue &value : QJsonDocument::fromJson(answer).object().value("results").toArray()) {
        const QJsonObject book = value.toObject();
        MetaCandidate candidate;
        candidate.source = "Apple Books";
        candidate.meta.title = book.value("trackName").toString();
        candidate.meta.authors = BookMeta::splitAuthors(book.value("artistName").toString());
        candidate.meta.published = book.value("releaseDate").toString().left(10);
        candidate.meta.description = htmlToText(book.value("description").toString());
        // "Books" is the shop's own shelf, not a genre.
        for (const QString &genre : strings(book.value("genres"), 6))
            if (genre != "Books")
                candidate.meta.tags << genre;
        // The address names the size; the shop serves any.
        QString cover = book.value("artworkUrl100").toString();
        if (!cover.isEmpty())
            candidate.coverUrl = QUrl(cover.replace("100x100bb", "900x900bb"));
        if (!candidate.meta.title.isEmpty())
            candidates << candidate;
    }
    return candidates;
}

QUrl CrossrefSource::searchUrl(const BookMeta &book) const {
    QList<QPair<QString, QString>> items{{"rows", "4"}, {"select", "title,subtitle,author,publisher,issued,ISBN,subject,abstract"}};
    const QString books = "type:book,type:monograph,type:edited-book,type:reference-book";
    if (!book.isbn.isEmpty()) {
        items.emplaceBack("filter", books + ",isbn:" + book.isbn);
    } else {
        items.emplaceBack("filter", books);
        items.emplaceBack("query.title", book.title);
        if (!book.authors.isEmpty())
            items.emplaceBack("query.author", book.authors.first());
    }
    return searchAddress("https://api.crossref.org/works", items);
}

QList<MetaCandidate> CrossrefSource::parseAnswer(const QByteArray &answer) {
    QList<MetaCandidate> candidates;
    const QJsonArray items = QJsonDocument::fromJson(answer).object().value("message").toObject().value("items").toArray();
    for (const QJsonValue &value : items) {
        const QJsonObject work = value.toObject();
        MetaCandidate candidate;
        candidate.source = "Crossref";
        candidate.meta.title = strings(work.value("title")).value(0);
        if (const QString subtitle = strings(work.value("subtitle")).value(0); !subtitle.isEmpty())
            candidate.meta.title += ": " + subtitle;
        for (const QJsonValue &author : work.value("author").toArray()) {
            const QString name = (author.toObject().value("given").toString() + " " + author.toObject().value("family").toString()).simplified();
            if (!name.isEmpty())
                candidate.meta.authors << name;
        }
        candidate.meta.publisher = work.value("publisher").toString();
        // [[2013, 4, 6]], with the month and day often left off.
        QStringList date;
        for (const QJsonValue &part : work.value("issued").toObject().value("date-parts").toArray().first().toArray())
            date << QString::number(part.toInt()).rightJustified(date.isEmpty() ? 4 : 2, '0');
        candidate.meta.published = date.join('-');
        for (const QString &isbn : strings(work.value("ISBN")))
            if (candidate.meta.isbn.isEmpty() || isbn.startsWith("978"))
                candidate.meta.isbn = isbn;
        candidate.meta.tags = strings(work.value("subject"), 5);
        candidate.meta.description = htmlToText(work.value("abstract").toString());
        if (!candidate.meta.title.isEmpty())
            candidates << candidate;
    }
    return candidates;
}

ProcessMetadataSource::ProcessMetadataSource(const QString &name, const QStringList &fields, const QString &program,
                                             const QStringList &arguments, const QString &folder, QObject *parent)
    : MetadataSource(parent), m_name(name), m_program(program), m_folder(folder), m_fields(fields), m_arguments(arguments) {
    // A plugin that never answers must not hold a look-up open for ever.
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(45000);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        m_timedOut = true;
        m_process.kill();
    });
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_timeout.stop();
        QString problem;
        const QList<MetaCandidate> candidates = read(m_process.readAllStandardOutput(), &problem);
        const QString said = QString::fromUtf8(m_process.readAllStandardError()).trimmed().section('\n', -1);
        if (m_timedOut)
            emit failed(Failure(Failure::Code::SourceTooSlow, {m_name}, m_timeout.interval() / 1000));
        else if (!candidates.isEmpty() || (code == 0 && status == QProcess::NormalExit && problem.isEmpty()))
            emit found(candidates);
        else
            emit failed(!problem.isEmpty() || !said.isEmpty()
                            ? Failure(Failure::Code::SourceSaid, {m_name, !problem.isEmpty() ? problem : said})
                            : Failure(Failure::Code::SourceStopped, {m_name}));
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_timeout.stop();
            emit failed(Failure(Failure::Code::ProgramWontStart, {m_name, m_program}));
        }
    });
}

void ProcessMetadataSource::search(const BookMeta &book) {
    m_timedOut = false;
    if (!m_folder.isEmpty())
        m_process.setWorkingDirectory(m_folder);
    m_process.start(m_program, m_arguments);
    m_process.write(request(book));
    m_process.closeWriteChannel();
    m_timeout.start();
}

QByteArray ProcessMetadataSource::request(const BookMeta &book) const {
    return QJsonDocument(book.toJson()).toJson(QJsonDocument::Compact) + "\n";
}

QList<MetaCandidate> ProcessMetadataSource::read(const QByteArray &output, QString *problem) const {
    QList<MetaCandidate> candidates;
    const QJsonObject answer = QJsonDocument::fromJson(output).object();
    *problem = answer.value("error").toString();
    for (const QJsonValue &value : answer.value("candidates").toArray()) {
        const MetaCandidate candidate = MetaCandidate::fromJson(value.toObject(), m_name);
        if (!candidate.meta.title.isEmpty())
            candidates << candidate;
    }
    return candidates;
}

CalibreMetadataSource::CalibreMetadataSource(const QString &plugin, const QStringList &fields, const QString &calibreDebug,
                                             const QString &bridge, QObject *parent)
    : ProcessMetadataSource(plugin + " · Calibre", fields, calibreDebug, {"-e", bridge, "--", "identify", "/dev/stdin"}, {}, parent),
      m_plugin(plugin) {}

QByteArray CalibreMetadataSource::request(const BookMeta &book) const {
    QJsonObject request = book.toJson();
    request.insert("plugins", QJsonArray{m_plugin});
    return QJsonDocument(request).toJson(QJsonDocument::Compact);
}

// Calibre prints what it likes on standard output; the bridge's answer is the
// line that starts with its marker.
QList<MetaCandidate> CalibreMetadataSource::read(const QByteArray &output, QString *problem) const {
    QList<MetaCandidate> candidates;
    const qsizetype at = output.lastIndexOf("OMABOOK-JSON:");
    if (at < 0) {
        *problem = "Calibre gave no answer.";
        return candidates;
    }
    const QByteArray line = output.mid(at + 13).split('\n').first();
    for (const QJsonValue &answer : QJsonDocument::fromJson(line).array()) {
        *problem = answer.toObject().value("error").toString();
        for (const QJsonValue &value : answer.toObject().value("candidates").toArray()) {
            const MetaCandidate candidate = MetaCandidate::fromJson(value.toObject(), name());
            if (!candidate.meta.title.isEmpty())
                candidates << candidate;
        }
    }
    return candidates;
}

AiMetadataSource::AiMetadataSource(AiProvider *provider, const QString &fileName, const QString &opening, QObject *parent)
    : MetadataSource(parent), m_provider(provider), m_fileName(fileName), m_opening(opening) {
    connect(provider, &AiProvider::delta, this, [this](const QString &text) { m_answer += text; });
    connect(provider, &AiProvider::failed, this, &MetadataSource::failed);
    connect(provider, &AiProvider::finished, this, [this] {
        MetaCandidate candidate = Prompts::parseMetadata(m_answer, m_found);
        candidate.source = name();
        // Where the model named no cover, the one of the result it took the
        // title from comes along.
        for (const MetaCandidate &found : std::as_const(m_found))
            if (!candidate.coverUrl.isValid() && found.coverUrl.isValid()
                && found.meta.title.compare(candidate.meta.title, Qt::CaseInsensitive) == 0)
                candidate.coverUrl = found.coverUrl;
        if (candidate.meta.title.isEmpty())
            emit failed(Failure(Failure::Code::NoDetailsFound, {name()}));
        else
            emit found({candidate});
    });
}

void AiMetadataSource::search(const BookMeta &book) {
    m_answer.clear();
    m_provider->ask(Prompts::metadata(m_fileName, book, m_opening, m_found));
}

void fetchCover(QNetworkAccessManager *network, const QUrl &url, QObject *context, std::function<void(const QImage &)> done) {
    QNetworkReply *reply = network->get(request(url));
    QObject::connect(reply, &QNetworkReply::finished, context, [reply, done] {
        reply->deleteLater();
        done(reply->error() == QNetworkReply::NoError ? QImage::fromData(reply->readAll()) : QImage());
    });
}
