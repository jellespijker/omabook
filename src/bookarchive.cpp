#include "bookarchive.h"

#include <QFile>
#include <archive.h>
#include <archive_entry.h>
#include <memory>

namespace {

// A book's largest honest member is a scanned page or an embedded font; this
// stops an archive that claims gigabytes from eating the machine's memory.
const la_int64_t maximumEntrySize = 512LL * 1024 * 1024;

struct Closer {
    void operator()(archive *a) const { archive_read_free(a); }
};
using Reader = std::unique_ptr<archive, Closer>;

Reader open(const QString &path) {
    Reader reader(archive_read_new());
    archive_read_support_filter_all(reader.get());
    archive_read_support_format_all(reader.get());
    if (archive_read_open_filename(reader.get(), QFile::encodeName(path).constData(), 1 << 16) != ARCHIVE_OK)
        return nullptr;
    return reader;
}

QString entryName(archive_entry *entry) {
    if (const char *utf8 = archive_entry_pathname_utf8(entry))
        return QString::fromUtf8(utf8);
    if (const wchar_t *wide = archive_entry_pathname_w(entry))
        return QString::fromWCharArray(wide);
    return QString::fromLocal8Bit(archive_entry_pathname(entry));
}

} // namespace

QStringList Archive::entries() const {
    QStringList names;
    Reader reader = open(m_path);
    if (!reader)
        return names;
    archive_entry *entry = nullptr;
    while (archive_read_next_header(reader.get(), &entry) == ARCHIVE_OK) {
        if (archive_entry_filetype(entry) == AE_IFREG)
            names << entryName(entry);
        archive_read_data_skip(reader.get());
    }
    return names;
}

QByteArray Archive::read(const QString &name) const {
    Reader reader = open(m_path);
    if (!reader)
        return {};
    archive_entry *entry = nullptr;
    while (archive_read_next_header(reader.get(), &entry) == ARCHIVE_OK) {
        if (entryName(entry) != name) {
            archive_read_data_skip(reader.get());
            continue;
        }
        if (archive_entry_size_is_set(entry) && archive_entry_size(entry) > maximumEntrySize)
            return {};
        QByteArray data;
        char buffer[1 << 16];
        for (;;) {
            const la_ssize_t count = archive_read_data(reader.get(), buffer, sizeof buffer);
            if (count < 0 || data.size() + count > maximumEntrySize)
                return {};
            if (count == 0)
                break;
            data.append(buffer, count);
        }
        // An empty file is still a file: an empty, not a null, array.
        return data.isNull() ? QByteArray("") : data;
    }
    return {};
}

bool Archive::write(const QString &path, const QList<QPair<QString, QByteArray>> &files) {
    struct Finisher {
        void operator()(archive *a) const { archive_write_free(a); }
    };
    std::unique_ptr<archive, Finisher> writer(archive_write_new());
    archive_write_set_format_zip(writer.get());
    bool written = archive_write_open_filename(writer.get(), QFile::encodeName(path).constData()) == ARCHIVE_OK;
    for (const auto &[name, data] : files) {
        if (!written)
            break;
        struct Freer {
            void operator()(archive_entry *e) const { archive_entry_free(e); }
        };
        const std::unique_ptr<archive_entry, Freer> entry(archive_entry_new());
        archive_entry_set_pathname_utf8(entry.get(), name.toUtf8().constData());
        archive_entry_set_size(entry.get(), data.size());
        archive_entry_set_filetype(entry.get(), AE_IFREG);
        archive_entry_set_perm(entry.get(), 0644);
        written = archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK
                  && archive_write_data(writer.get(), data.constData(), size_t(data.size())) == data.size();
    }
    written = archive_write_close(writer.get()) == ARCHIVE_OK && written;
    if (!written)
        QFile::remove(path);
    return written;
}
