#include "plugins/archive/ZipPrefix.h"

#include <archive.h>
#include <archive_entry.h>

namespace mole {
namespace {

    /// Closes and frees whatever happens next.
    class MemoryReader
    {
    public:
        explicit MemoryReader(QByteArrayView bytes)
            : m_handle(archive_read_new())
        {
            if (!m_handle)
                return;
            archive_read_support_filter_all(m_handle);
            archive_read_support_format_all(m_handle);
            m_opened = archive_read_open_memory(m_handle, bytes.data(), size_t(bytes.size())) == ARCHIVE_OK;
        }

        ~MemoryReader()
        {
            if (!m_handle)
                return;
            if (m_opened)
                archive_read_close(m_handle);
            archive_read_free(m_handle);
        }

        MemoryReader(const MemoryReader&) = delete;
        MemoryReader& operator=(const MemoryReader&) = delete;

        bool isOpen() const { return m_opened; }
        archive* handle() const { return m_handle; }

    private:
        archive* m_handle = nullptr;
        bool m_opened = false;
    };

} // namespace

QHash<QString, QByteArray> membersFromZipPrefix(
    QByteArrayView prefix, const QStringList& wanted, qint64 maxMemberBytes)
{
    QHash<QString, QByteArray> found;
    if (prefix.isEmpty() || wanted.isEmpty())
        return found;

    MemoryReader reader(prefix);
    if (!reader.isOpen())
        return found;

    archive_entry* entry = nullptr;
    while (archive_read_next_header(reader.handle(), &entry) == ARCHIVE_OK) {
        const char* rawName = archive_entry_pathname(entry);
        const QString name = rawName ? QString::fromUtf8(rawName) : QString();
        if (!wanted.contains(name)) {
            archive_read_data_skip(reader.handle());
            continue;
        }

        // Read what is there rather than what the header says is there: the
        // prefix may stop in the middle of this member, and a declared size is
        // a claim in a container as much as in an EXIF tag.
        QByteArray contents;
        QByteArray chunk(64 * 1024, Qt::Uninitialized);
        while (contents.size() < maxMemberBytes) {
            const la_ssize_t got = archive_read_data(reader.handle(), chunk.data(),
                size_t(std::min<qint64>(chunk.size(), maxMemberBytes - contents.size())));
            if (got <= 0)
                break;
            contents.append(chunk.constData(), qsizetype(got));
        }
        found.insert(name, contents);

        if (found.size() == wanted.size())
            break; // everything asked for is in hand
    }
    return found;
}

} // namespace mole
