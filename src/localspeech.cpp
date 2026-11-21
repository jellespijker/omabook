#include "localspeech.h"

#include <QFileInfo>
#include <QStandardPaths>

using namespace Qt::StringLiterals;

QString LocalSpeech::found(const QString &program) {
    if (QFileInfo(program).isAbsolute())
        return QFileInfo(program).isExecutable() ? program : QString();
    return QStandardPaths::findExecutable(program);
}

bool LocalSpeech::isWav(const QString &file) { return file.endsWith(".wav"_L1, Qt::CaseInsensitive); }

QStringList LocalSpeech::conversion(const QString &recording, const QString &wav) {
    return {u"-nostdin"_s, u"-loglevel"_s, u"error"_s, u"-y"_s,   u"-i"_s,        recording, u"-ar"_s,
            u"16000"_s,    u"-ac"_s,       u"1"_s,     u"-c:a"_s, u"pcm_s16le"_s, wav};
}
