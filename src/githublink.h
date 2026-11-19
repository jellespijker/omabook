#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <expected>
#include <memory>

#include "pluginsources.h"
#include "spacetransport.h"

// A space's snapshot kept on GitHub (#7): in a gist, or as a file of a
// repository. One transport of a space (SpaceTransport).
//
// Reading asks GitHub the way plugin installs do (PluginSources): over HTTPS
// to GitHub only, with the token the plugin settings keep if there is one,
// and none needed for a public repository or a gist, secret or not, whose
// link one was given. Sending goes through GitHub's own command line, `gh`,
// which holds the person's account: Omabook keeps no account or token of its
// own for it. A space is sent as a gist, secret unless asked otherwise, with
// one file, `collection.omabook-space`; sending it again brings that gist up
// to date, so its link stays what others follow.
class GitHubLink final : public SpaceTransport {
public:
    // A link as it was given: a gist by its id or any of its addresses
    //   https://gist.github.com/<id>   https://gist.github.com/<owner>/<id>[/<revision>]
    //   https://gist.githubusercontent.com/<owner>/<id>/raw/…   <id>
    // or a repository's file
    //   https://github.com/<owner>/<repo>/blob/<ref>/<path>
    //   https://raw.githubusercontent.com/<owner>/<repo>/<ref>/<path>
    // A ref with a slash in it can't be told from the path in an address.
    struct Address {
        QString gist; // its id, for a gist
        QString owner, repo, ref, path;
        [[nodiscard]] bool isGist() const { return !gist.isEmpty(); }
    };
    [[nodiscard]] static std::expected<Address, QString> parse(const QString &text);

    // The file a gist made here holds.
    static constexpr auto fileName = "collection.omabook-space";

    // `gh` is the command line that sends, found on PATH unless named; the
    // service is where GitHub is (a stand-in for the tests).
    explicit GitHubLink(QString gh = QStringLiteral("gh"),
                        const PluginSources::Service &service = PluginSources::Service::fromEnvironment());
    ~GitHubLink() override;
    GitHubLink(const GitHubLink &) = delete;
    GitHubLink &operator=(const GitHubLink &) = delete;
    GitHubLink(GitHubLink &&) = delete;
    GitHubLink &operator=(GitHubLink &&) = delete;

    [[nodiscard]] QString name() const override { return QStringLiteral("github"); }
    [[nodiscard]] bool handles(const QString &address) const override { return parse(address).has_value(); }
    [[nodiscard]] std::expected<SpaceSnapshot, QString> fetch(const QString &address) override;
    [[nodiscard]] std::expected<Sent, QString> publish(const SpaceSnapshot &snapshot, const Sending &sending) override;

private:
    // What `gh` answered: how it ended, what it printed and what it said was wrong.
    struct Answer {
        int exit = 0;
        QByteArray output;
        QString errors;
    };
    [[nodiscard]] std::expected<Answer, QString> gh(const QStringList &arguments, const QByteArray &input) const;
    [[nodiscard]] std::expected<QByteArray, QString> get(const QUrl &url, const QByteArray &accept, const QString &missing) const;
    [[nodiscard]] std::expected<SpaceSnapshot, QString> fetchGist(const QString &id) const;
    [[nodiscard]] std::expected<Sent, QString> send(const QString &gist, const SpaceSnapshot &snapshot, const Sending &sending) const;

    QString m_gh;
    PluginSources::Service m_service;
    std::unique_ptr<PluginSources> m_sources;
};
