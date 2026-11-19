#pragma once

#include <QPointer>
#include <QString>
#include <QStringList>

#include "metadatasource.h"
#include "scriptplugin.h"

// A source of details that is a script's `search` function.
class ScriptMetadataSource : public MetadataSource {
    Q_OBJECT

public:
    ScriptMetadataSource(ScriptPlugin *plugin, QString name, QStringList fields, QObject *parent = nullptr);

    QString name() const override { return m_name; }
    QStringList fields() const override { return m_fields; }
    void search(const BookMeta &book) override;

private:
    // The plugin may be loaded anew, or switched off, while a look-up runs.
    QPointer<ScriptPlugin> m_plugin;
    QString m_name;
    QStringList m_fields;
};
