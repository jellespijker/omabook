#include "scriptmetadatasource.h"

#include <QJsonArray>
#include <QTimer>
#include <utility>

ScriptMetadataSource::ScriptMetadataSource(ScriptPlugin *plugin, QString name, QStringList fields, QObject *parent)
    : MetadataSource(parent), m_plugin(plugin), m_name(std::move(name)), m_fields(std::move(fields)) {}

void ScriptMetadataSource::search(const BookMeta &book) {
    // Answered through the event loop even when the script answers at once
    // or isn't there: whoever asked is still connecting.
    const auto answer = [this](const QList<MetaCandidate> &candidates, const QString &problem) {
        QTimer::singleShot(0, this, [this, candidates, problem] {
            if (problem.isEmpty())
                emit found(candidates);
            else
                emit failed(Failure(Failure::Code::SourceSaid, {m_name, problem}));
        });
    };
    if (!m_plugin) {
        answer({}, "The plugin was switched off or changed while it was being asked.");
        return;
    }
    m_plugin->search(m_name, book.toJson(),
                     [this, guard = QPointer<ScriptMetadataSource>(this), answer](const QJsonValue &result, const QString &problem) {
                         if (!guard)
                             return;
                         QList<MetaCandidate> candidates;
                         for (const auto &value : result.toArray()) {
                             const MetaCandidate candidate = MetaCandidate::fromJson(value.toObject(), m_name);
                             if (!candidate.meta.title.isEmpty())
                                 candidates << candidate;
                         }
                         answer(candidates, problem.isEmpty() && !result.isArray() && !result.isNull()
                                                ? "search should answer a list of candidates."
                                                : problem);
                     });
}
