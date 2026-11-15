#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <functional>
#include <memory>

#include "ai.h"
#include "aisearch.h"
#include "curation.h"
#include "customization.h"
#include "readingsite.h"
#include "sitedesk.h"

class Backend;
class Settings;

// The AI at the level of the whole library and the app itself, for the
// window: tidying the library as one batch to review, answering questions
// about what it holds, and changing the interface when asked to.
class Librarian : public QObject {
    Q_OBJECT
    // The changes proposed by a tidy, each with whether it is chosen.
    Q_PROPERTY(QVariantList proposals READ proposals NOTIFY proposalsChanged)
    Q_PROPERTY(int chosen READ chosen NOTIFY proposalsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString status READ status NOTIFY busyChanged)
    // The answer to a question about the library, as it is written.
    Q_PROPERTY(QString answer READ answer NOTIFY answerChanged)
    Q_PROPERTY(bool answering READ answering NOTIFY answerChanged)
    // What the AI suggested to read next, each looked up on the reading
    // sites: whether it exists, its readers' rating, whether it is here.
    Q_PROPERTY(QVariantList suggestions READ suggestions NOTIFY answerChanged)
    // The reading sites and how the reader is connected to each: a SiteDesk
    // (sitedesk.h), which is where connecting, syncing and the rest live.
    Q_PROPERTY(QObject *sites READ sites CONSTANT)
    // What the last change to the interface did, or why it couldn't be made.
    Q_PROPERTY(QString changeNote READ changeNote NOTIFY changeChanged)
    Q_PROPERTY(bool changing READ changing NOTIFY changeChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changeChanged)
    // Whether asking about the library and for suggestions keeps to the
    // collection the shelf shows. The knowledge graph's picture of the whole
    // library goes along either way.
    Q_PROPERTY(bool onlyCollection READ onlyCollection WRITE setOnlyCollection NOTIFY answerChanged)
    // Books found for a question in the reader's own words (aisearch.h), the
    // best first: [{"bookId", "title", "authors", "cover", "why", "readable"}].
    // While it looks, `finding` and a status ("Looked through 240 of 1,600
    // books…"); a failure in plain words; whether the answer was kept from
    // asking the same before.
    Q_PROPERTY(QVariantList found READ found NOTIFY foundChanged)
    Q_PROPERTY(QString findQuestion READ findQuestion NOTIFY foundChanged)
    Q_PROPERTY(bool finding READ finding NOTIFY foundChanged)
    Q_PROPERTY(QString findStatus READ findStatus NOTIFY foundChanged)
    Q_PROPERTY(QString findError READ findError NOTIFY foundChanged)
    Q_PROPERTY(bool findCached READ findCached NOTIFY foundChanged)
    Q_PROPERTY(bool hasAi READ hasAi NOTIFY aiChanged)
    Q_PROPERTY(QString aiProblem READ aiProblem NOTIFY aiChanged)

public:
    using Colors = std::function<QVariantMap()>;
    Librarian(Backend *backend, Settings *settings, Customization *custom, Colors colors, QObject *parent = nullptr);
    ~Librarian() override;

    QVariantList proposals() const;
    int chosen() const;
    bool busy() const noexcept { return m_busy; }
    QString status() const noexcept { return m_status; }
    QString answer() const noexcept { return m_answer; }
    bool answering() const noexcept { return m_answering; }
    QVariantList suggestions() const;
    QObject *sites() noexcept { return &m_desk; }
    SiteDesk *siteDesk() noexcept { return &m_desk; }
    // The reading sites to ask, each owned by the parent it is given. Tests
    // pass stand-ins; otherwise the settings decide.
    using Sites = std::function<QList<ReadingSite *>(QObject *parent)>;
    void setSites(Sites sites) { m_sites = std::move(sites); }
    QString changeNote() const noexcept { return m_changeNote; }
    bool changing() const noexcept { return m_changing; }
    bool canUndo() const;
    bool hasAi() const;
    QString aiProblem() const;

    // Looks the library over. Duplicates and author spellings are found at
    // once; with an AI, series are too; and with `lookUps`, the details of
    // books that have no description or cover are looked up and judged.
    Q_INVOKABLE void tidy(bool lookUps = false);
    Q_INVOKABLE void setChosen(int index, bool chosen);
    Q_INVOKABLE void applyChosen();
    Q_INVOKABLE void stop();
    // How many books a tidy with look-ups would look up.
    Q_INVOKABLE int incomplete() const;
    // Has the AI propose a collection for each book that is in none, a
    // batch at a time (Curation::uncollected), as proposals like the rest:
    // nothing changes until they are applied. Joins a tidy that is shown.
    Q_INVOKABLE void sortIntoCollections();
    // How many books are in no collection and could be sorted, at most
    // what one pass takes; 0 when the library has no collections.
    Q_INVOKABLE int uncollected() const;
    bool onlyCollection() const noexcept { return m_onlyCollection; }
    void setOnlyCollection(bool only);

    QVariantList found() const;
    QString findQuestion() const noexcept { return m_findQuestion; }
    bool finding() const noexcept { return m_finding; }
    QString findStatus() const noexcept { return m_findStatus; }
    QString findError() const noexcept { return m_findError; }
    bool findCached() const noexcept { return m_findCached; }
    // Has the AI find the books, of the collection shown, that answer a
    // question; a question asked before of the library as it is now is
    // answered from what was kept.
    Q_INVOKABLE void find(const QString &question);
    Q_INVOKABLE void stopFinding();

    Q_INVOKABLE void ask(const QString &question);
    // What to read next that the library doesn't hold, from what was read.
    Q_INVOKABLE void suggest();
    // Puts a suggestion on the to-read shelf, as a book only known of.
    Q_INVOKABLE void wantSuggestion(int index);

    // Asks for the file Goodreads lets its users download, and brings it in.
    Q_INVOKABLE void importSiteFile();
    // Asks where to write the library's reading as a file for Goodreads' import page.
    Q_INVOKABLE void exportSiteFile();
    Q_INVOKABLE void change(const QString &wish);
    Q_INVOKABLE void undoChange();

signals:
    void proposalsChanged();
    void busyChanged();
    void answerChanged();
    void changeChanged();
    void foundChanged();
    void aiChanged();

private:
    struct Entry {
        Proposal proposal;
        bool chosen = true;
    };
    void setBusy(bool busy, const QString &status = {});
    void add(const QList<Proposal> &proposals);
    void askSeries();
    void askCollections();
    // The books asking and suggesting go by: the shown collection's, or all.
    QList<Book> askedBooks() const;
    void lookUpNext();
    void finishTidy();
    void startAnswer(std::unique_ptr<AiProvider> provider, const AiRequest &request);
    void checkSuggestions(const Curation::Suggestions &suggested);
    void readingFilePicked(bool save, const QString &file);

    Backend *m_backend;
    Settings *m_settings;
    Customization *m_custom;
    Colors m_colors;
    QList<Entry> m_entries;
    bool m_busy = false;
    bool m_lookUps = false;
    QString m_status;
    std::unique_ptr<AiCall> m_tidyCall;
    // The batches of books still to be sorted into collections, and how many there were.
    QList<QList<Book>> m_toSort;
    int m_sortTotal = 0;
    bool m_onlyCollection = false;
    // Books still to look up, and the look-up in hand.
    QList<qint64> m_toLookUp;
    int m_lookUpTotal = 0;
    std::unique_ptr<QObject> m_lookUp;
    std::unique_ptr<AiProvider> m_judge;

    std::unique_ptr<AiCall> m_askCall;
    QString m_answer;
    // The question being answered, to be written into the knowledge graph.
    QString m_question;
    bool m_answering = false;
    // What asks the reading sites, declared before everything that uses it.
    Sites m_sites;
    QNetworkAccessManager m_network;
    CoverFetcher m_covers{&m_network};
    int m_coversArrived = 0;
    // The suggestions being checked, and the sites they are checked with.
    QList<ReadingSites::Checked> m_suggestions;
    std::unique_ptr<QObject> m_checking;
    QPointer<SuggestionCheck> m_check;

    SiteDesk m_desk;

    // Finding books by a question: the question, what was found, and the
    // search under way (deleted later, as it may end inside its own call).
    void startFinding(std::shared_ptr<const KnowledgeIndex> known, const QList<Book> &books, const QString &key);
    void finishFinding(const QList<AiSearch::Pick> &picks, const QString &key);
    QString m_findQuestion;
    QList<AiSearch::Pick> m_found;
    bool m_finding = false;
    QString m_findStatus, m_findError;
    bool m_findCached = false;
    int m_findAsked = 0;
    QPointer<AiSearch::Run> m_findRun;

    std::unique_ptr<AiCall> m_changeCall;
    QString m_changeNote;
    bool m_changing = false;
};
