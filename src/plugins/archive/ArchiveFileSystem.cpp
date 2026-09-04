#include "plugins/archive/ArchiveFileSystem.h"

#include "core/data/FileType.h"
#include "core/vfs/PathWords.h"

#include <QBuffer>
#include <QFileInfo>
#include <QMutexLocker>
#include <QUrl>

#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <locale.h>

namespace mole {
namespace {

    constexpr int kReadBlockSize = 64 * 1024;

    /// A UTF-8 LC_CTYPE for as long as libarchive is being called, on this
    /// thread and no other.
    ///
    /// libarchive converts the names in a header to the **process locale**, and
    /// when it cannot it answers ARCHIVE_WARN and stores no pathname at all --
    /// which in a `C` locale is every non-ASCII name there is. Measured rather
    /// than assumed, because the obvious answer does not work:
    /// archive_entry_pathname_utf8() returns null there too, so there is nothing
    /// left to fall back to and the member cannot be named. Under
    /// LC_CTYPE=C.UTF-8 the same header reads cleanly, with no warning.
    ///
    /// So it is the conversion that is wrong here, not the reading of it. A `C`
    /// locale is not exotic: the second-family CI job runs with no locale set and
    /// the AppImage inherits whatever the host has, so a zip written on Windows
    /// with accented names listed as a truncated tree -- and "copy everything
    /// out" copied a subset. See MOLE-352.
    ///
    /// Per call and per thread. The process locale is not this plugin's to
    /// change: Qt does its own conversions and does not consult it, and a
    /// setlocale() here would be reaching under everything else in the process
    /// from whichever thread got here first. uselocale() is a thread-local
    /// pointer swap and costs nothing.
    class Utf8Names
    {
    public:
#ifdef Q_OS_UNIX
        Utf8Names()
            : m_previous(uselocale(utf8Ctype()))
        {
        }
        ~Utf8Names()
        {
            if (m_previous != static_cast<locale_t>(0))
                uselocale(m_previous);
        }
#else
        // Windows has no uselocale(), and libarchive there reads names through
        // the wide-character API rather than converting through LC_CTYPE, so
        // there is nothing to correct. Whether that is the whole story is a
        // question for the machine in MOLE-253.
        Utf8Names() = default;
        ~Utf8Names() = default;
#endif
        Utf8Names(const Utf8Names&) = delete;
        Utf8Names& operator=(const Utf8Names&) = delete;

    private:
#ifdef Q_OS_UNIX
        /// Made once. Null if this system has no UTF-8 locale at all, in which
        /// case uselocale(0) reports the current one and changes nothing --
        /// there is no better answer available and no reason to fail over it.
        static locale_t utf8Ctype()
        {
            static locale_t made = [] {
                for (const char* name : { "C.UTF-8", "C.utf8", "en_US.UTF-8" }) {
                    if (locale_t built = newlocale(LC_CTYPE_MASK, name, static_cast<locale_t>(0)))
                        return built;
                }
                return static_cast<locale_t>(0);
            }();
            return made;
        }

        locale_t m_previous;
#endif
    };

    /// RAII for libarchive's read handle, so no early return can leak it.
    class ArchiveReader
    {
    public:
        /// What the handle bids with.
        enum class Formats {
            /// Everything archive_read_support_format_all() covers: zip, tar, 7z,
            /// rar, iso, cpio, ar and the rest.
            Containers,
            /// `raw` alone: a stream with no container inside it, a file
            /// compressed with gzip rather than a tarball.
            ///
            /// Alone, and not alongside the containers, which matters. Some of
            /// them bid on text -- the mtree bidder claims a gzipped CSV -- and a
            /// bid beats raw's, which is the lowest there is. The container would
            /// then win and fail at the first header, and the file would look like
            /// an archive of nothing. By the time this is asked for, the containers
            /// have already been asked and have answered no, so the only question
            /// left is whether this is a single compressed stream.
            SingleStreamOnly,
        };

        ArchiveReader(const QString& path, Formats formats)
        {
            m_handle = archive_read_new();
            if (!m_handle)
                return;
            archive_read_support_filter_all(m_handle);
            // libarchive keeps `raw` out of support_format_all() on purpose: it
            // bids lowest, so a real container still wins, but anything no format
            // claims becomes an archive of one unnamed member -- a plain text file
            // included. Asked for on its own, and only once the containers have
            // refused the file. See openArchive().
            if (formats == Formats::SingleStreamOnly)
                archive_read_support_format_raw(m_handle);
            else
                archive_read_support_format_all(m_handle);
            const Utf8Names names;
            m_opened = archive_read_open_filename(m_handle, path.toLocal8Bit().constData(), kReadBlockSize)
                == ARCHIVE_OK;
        }

        ~ArchiveReader()
        {
            if (!m_handle)
                return;
            if (m_opened)
                archive_read_close(m_handle);
            archive_read_free(m_handle);
        }

        ArchiveReader(const ArchiveReader&) = delete;
        ArchiveReader& operator=(const ArchiveReader&) = delete;

        bool isOpen() const { return m_handle && m_opened; }

        /// Whether any format claimed this file, which the open alone does not
        /// settle: libarchive may accept the file and discover only at the first
        /// header that nothing recognises it. Which of the two happens depends on
        /// how far the format bidders had to read -- a gzipped line of prose is
        /// refused at the open and a gzipped CSV at the first header, and before
        /// this was understood the second one fell through every test and listed
        /// as an archive of nothing.
        bool hasEntries()
        {
            if (!isOpen())
                return false;
            if (!m_peeked && !m_exhausted) {
                const Utf8Names names;
                // ARCHIVE_OK and nothing else, which is the opposite of the walk
                // below and deliberate. This decides whether any format claimed
                // the file at all, and a format that has to complain about its
                // very first header has not: the mtree bidder answers WARN for
                // every line of a gzipped CSV and would otherwise win, listing a
                // price list as an archive of three members. The name conversion
                // that used to warn here does not any more -- see Utf8Names --
                // so what is left really is a bidder that does not fit.
                m_peeked = archive_read_next_header(m_handle, &m_first) == ARCHIVE_OK;
                m_lastHeader = m_peeked ? ARCHIVE_OK : ARCHIVE_FATAL;
                if (!m_peeked)
                    m_exhausted = true;
            }
            return m_peeked;
        }

        /// The next entry, or null at the end. The first call hands back the
        /// header hasEntries() had to read, rather than skipping past it.
        ///
        /// Null means "no more", which is two different things -- see
        /// endedAtTheEnd(). A caller building an index has to ask which.
        struct archive_entry* nextHeader()
        {
            if (m_peeked) {
                m_peeked = false;
                return m_first;
            }
            if (!isOpen() || m_exhausted)
                return nullptr;
            const Utf8Names names;
            struct archive_entry* entry = nullptr;
            if (!readAHeader(archive_read_next_header(m_handle, &entry))) {
                m_exhausted = true;
                return nullptr;
            }
            return entry;
        }

        /// Whether the walk stopped because the archive ended, rather than
        /// because reading it failed. Meaningful once nextHeader() has answered
        /// null, and the difference between an archive listed whole and one
        /// listed as far as its damage. See MOLE-352.
        bool endedAtTheEnd() const { return m_lastHeader == ARCHIVE_EOF; }

        /// The format the reader settled on, once a header has been read.
        int format() const { return isOpen() ? archive_format(m_handle) : 0; }
        bool isZip() const { return (format() & ARCHIVE_FORMAT_BASE_MASK) == ARCHIVE_FORMAT_ZIP; }
        /// Whether something actually decompressed this: gzip, xz, bzip2, zstd.
        /// A file named `.gz` that is not gzip opens with no filter at all, and
        /// with `raw` enabled its own bytes would be offered as a member.
        bool wasDecompressed() const
        {
            return isOpen() && archive_filter_code(m_handle, 0) != ARCHIVE_FILTER_NONE;
        }
        struct archive* handle() const { return m_handle; }
        QString errorText() const
        {
            const char* message = m_handle ? archive_error_string(m_handle) : nullptr;
            return message ? QString::fromLocal8Bit(message) : QStringLiteral("unknown archive error");
        }

    private:
        /// Whether that return value handed over an entry.
        ///
        /// **ARCHIVE_WARN is a header that was read**, with a complaint
        /// attached. Testing for ARCHIVE_OK alone made the first complaint the
        /// end of the archive: the index stopped there, m_indexed was recorded
        /// as if the whole thing had been read, and "copy everything out" copied
        /// a subset of a tree nothing said was incomplete. A short listing is
        /// what a mirror deletes against. See MOLE-352.
        ///
        /// For the *first* header the rule is the other one -- see hasEntries(),
        /// which is not asking the same question.
        bool readAHeader(int result)
        {
            m_lastHeader = result;
            return result == ARCHIVE_OK || result == ARCHIVE_WARN;
        }

        struct archive* m_handle = nullptr;
        bool m_opened = false;
        /// What the last attempt to read a header answered, kept so that the end
        /// of the archive can be told from a failure to read it.
        int m_lastHeader = ARCHIVE_OK;
        /// The first header, read to find out whether anything claimed the file
        /// and held until somebody asks for it. libarchive reuses the entry, so it
        /// is handed over before another header is ever asked for.
        struct archive_entry* m_first = nullptr;
        bool m_peeked = false;
        bool m_exhausted = false;
    };

    /// libarchive reports "dir/sub/file.txt"; normalise to "/dir/sub/file.txt".
    ///
    /// "." and ".." are resolved here and stop at the root, which is the whole
    /// of this mount's defence against a hostile archive. An entry called
    /// "../../etc/passwd" is an instruction to write outside wherever it is
    /// extracted to, and one called "/etc/passwd" is the same instruction
    /// spelled differently; both become paths inside the archive and can address
    /// nothing else. It is also what keeps the tree acyclic -- "/.." resolves to
    /// the root, so a directory listing that contained it would list itself, and
    /// anything walking the mount would go round for ever.
    /// `windowsSeparators` says whether a backslash in this format's names is a
    /// separator. It is for a zip written by an old Windows tool, and it is not
    /// for a tar from Unix, where `a\\b.txt` is one perfectly ordinary name --
    /// which the unconditional rewrite turned into a folder called `a` holding a
    /// file called `b.txt`. See MOLE-352.
    QString normaliseEntryPath(const QString& raw, bool windowsSeparators)
    {
        QString raw_path = raw;
        if (windowsSeparators)
            raw_path.replace(QLatin1Char('\\'), QLatin1Char('/'));

        QStringList parts;
        for (const QString& part : raw_path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
            if (part == QLatin1String("."))
                continue;
            if (part == QLatin1String("..")) {
                if (!parts.isEmpty())
                    parts.removeLast();
                continue;
            }
            parts.append(part);
        }
        if (parts.isEmpty())
            return {};
        return QLatin1Char('/') + parts.join(QLatin1Char('/'));
    }

    /// An open archive, and whether it turned out to be a single compressed
    /// stream rather than a container.
    struct OpenArchive
    {
        std::unique_ptr<ArchiveReader> reader;
        bool singleStream = false;
    };

    /// Opens the archive, falling back to libarchive's `raw` format only when
    /// every container has refused it *and* the name promises a single compressed
    /// stream *and* something actually decompressed it.
    ///
    /// All three conditions matter. Enabled from the start, `raw` would turn
    /// "this is not an archive" into "an archive of one thing called data" for
    /// every unrecognised file, and the browser depends on that error. Without the
    /// suffix test, a container Mole cannot read would be offered as its own
    /// compressed bytes. Without the filter test, a file named `.gz` that is not
    /// gzip would open and show its raw bytes as a member.
    ///
    /// Container behaviour is untouched by construction: nothing here runs unless
    /// the ordinary open has already failed.
    OpenArchive openArchive(const QString& path)
    {
        auto containers = std::make_unique<ArchiveReader>(path, ArchiveReader::Formats::Containers);
        if (containers->hasEntries() || !FileType::namesSingleCompressedStream(QFileInfo(path).fileName()))
            return { std::move(containers), false };

        auto stream = std::make_unique<ArchiveReader>(path, ArchiveReader::Formats::SingleStreamOnly);
        if (stream->hasEntries() && stream->wasDecompressed())
            return { std::move(stream), true };
        // Whatever went wrong, the container reader holds the error worth showing:
        // "unrecognised archive format" rather than something about `raw`.
        return { std::move(containers), false };
    }

    /// What to call the one member of a single compressed stream.
    ///
    /// gzip stores the original filename and libarchive hands it over, so
    /// `notes.txt.gz` usually holds `notes.txt`. The field is optional -- `gzip -n`
    /// omits it and a stream written by a library usually does -- and xz and
    /// bzip2 have no such field at all, in which case libarchive calls the member
    /// `data`. A member called `data` is the wrong answer when the archive's own
    /// name says what is in it, so the name is worked out from the filename
    /// instead: `n.txt.xz` holds `n.txt`.
    QString singleStreamMemberName(const QString& archivePath, const QString& fromHeader)
    {
        if (!fromHeader.isEmpty() && fromHeader != QLatin1String("data"))
            return fromHeader;
        const QString derived = QFileInfo(archivePath).completeBaseName();
        return derived.isEmpty() ? QStringLiteral("data") : derived;
    }

    /// A name out of a header, in UTF-8 where libarchive could convert it.
    ///
    /// The UTF-8 accessor first, and this is not a preference: the plain one
    /// converts to the process locale and answers **null** when it cannot, which
    /// under a `C` locale is every non-ASCII name -- so the name came back empty
    /// and the member was skipped as nameless. Raw bytes as the last resort,
    /// because a name read as mojibake still addresses the right member and no
    /// name at all does not.
    QString nameFromHeader(const char* utf8, const char* local)
    {
        if (utf8)
            return QString::fromUtf8(utf8);
        if (local)
            return QString::fromLocal8Bit(local);
        return {};
    }

    /// Where this entry sits inside the mount.
    QString pathOfEntry(
        struct archive_entry* entry, const QString& archivePath, bool singleStream, bool windowsSeparators)
    {
        const QString reported
            = nameFromHeader(archive_entry_pathname_utf8(entry), archive_entry_pathname(entry));
        return normaliseEntryPath(
            singleStream ? singleStreamMemberName(archivePath, reported) : reported, windowsSeparators);
    }

    /// One member of an archive, decompressed as it is read rather than all at
    /// once.
    ///
    /// The point of it is what it does not do: hold the member. openRead() used to
    /// append the whole thing to a QByteArray and hand back a QBuffer over it,
    /// with no cap, so opening a 20 GB member asked for 20 GB of memory before the
    /// caller saw its first byte -- and the viewers are built on the opposite
    /// promise. HexPreviewController's own header says "the file is never held,
    /// only the window being shown, so a 100 GB firmware image opens as fast as a
    /// 100 byte one", which was true over a local disk and quietly untrue over an
    /// archive. Since MOLE-216 a member is as likely to be a compressed log or a
    /// database dump as a document, so it stopped being survivable. See MOLE-218.
    ///
    /// A stream format has no random access, so reaching byte N still means
    /// decompressing the N bytes before it. That is a cost in time, and it was
    /// being paid in memory as well. Here only the chunk being handed over exists:
    /// a seek forward decompresses and discards, and a seek backwards starts the
    /// stream again from the beginning of the archive. Both are slow and neither
    /// grows with the file, which is the right trade for a container that cannot
    /// be addressed by offset -- and it keeps the random access the previews and
    /// the span loop are written against.
    class ArchiveMemberDevice final : public QIODevice
    {
    public:
        /// `ordinal` is which header this member is, counted from zero as the
        /// walk reads them -- not its name.
        ///
        /// Two entries can carry the same name: `zip -u` appends a new version
        /// rather than replacing the old one, and a rebuilt jar does the same.
        /// The index keeps the last of them, while this walk used to stop at the
        /// first name that matched -- so the listing described one member and
        /// every read handed back another, with a size() that did not match the
        /// bytes. See MOLE-352.
        ArchiveMemberDevice(QString archivePath, QString memberPath, qint64 knownSize, int ordinal)
            : m_archivePath(std::move(archivePath))
            , m_memberPath(std::move(memberPath))
            , m_knownSize(knownSize)
            , m_ordinal(ordinal)
        {
        }

        bool open(OpenMode mode) override
        {
            if (!mode.testFlag(QIODevice::ReadOnly))
                return false;
            if (!startOfMember())
                return false;
            return QIODevice::open(mode | QIODevice::Unbuffered);
        }

        bool isSequential() const override { return false; }

        /// What the index said, when it knew. A single compressed stream does not
        /// know its uncompressed length until it has been read, and says so with
        /// an unset size rather than with nought -- see ensureIndexed().
        qint64 size() const override { return m_knownSize >= 0 ? m_knownSize : QIODevice::size(); }

        bool seek(qint64 position) override
        {
            if (position < 0)
                return false;
            if (position < m_offset && !startOfMember())
                return false;
            if (position > m_offset && !discardTo(position))
                return false;
            return QIODevice::seek(position);
        }

        /// Meaningful once open() or a read has failed.
        VfsError failure() const { return m_failure; }

    protected:
        qint64 readData(char* data, qint64 maxSize) override
        {
            if (maxSize <= 0 || !m_reader || !m_reader->isOpen())
                return 0;

            qint64 got = 0;
            while (got < maxSize) {
                const la_ssize_t read
                    = archive_read_data(m_reader->handle(), data + got, static_cast<size_t>(maxSize - got));
                if (read < 0) {
                    m_failure = VfsError::make(VfsError::IoError,
                        QStringLiteral("Cannot read %1: %2").arg(m_memberPath, m_reader->errorText()));
                    setErrorString(m_failure.message);
                    return -1;
                }
                if (read == 0)
                    break; // the end of the member, which is not a failure
                got += read;
            }
            m_offset += got;
            return got;
        }

        qint64 writeData(const char*, qint64) override { return -1; }

    private:
        /// Opens the archive and walks the headers to this member. Called again to
        /// go backwards, which is the only way a stream format can.
        bool startOfMember()
        {
            m_open = openArchive(m_archivePath);
            m_reader = m_open.reader.get();
            m_offset = 0;
            if (!m_reader || !m_reader->isOpen()) {
                m_failure = VfsError::make(VfsError::IoError,
                    QStringLiteral("Cannot open archive %1: %2")
                        .arg(m_archivePath, m_reader ? m_reader->errorText() : QString()));
                setErrorString(m_failure.message);
                return false;
            }

            int at = 0;
            while (struct archive_entry* entry = m_reader->nextHeader()) {
                const bool isTheOne = m_ordinal >= 0
                    ? at == m_ordinal
                    : pathOfEntry(entry, m_archivePath, m_open.singleStream, m_reader->isZip())
                        == m_memberPath;
                if (isTheOne)
                    return true;
                ++at;
                archive_read_data_skip(m_reader->handle());
            }

            m_failure = VfsError::make(
                VfsError::NotFound, QStringLiteral("Entry vanished from archive: %1").arg(m_memberPath));
            setErrorString(m_failure.message);
            m_reader = nullptr;
            return false;
        }

        /// Decompresses and throws away, which is what a forward seek costs.
        /// Reaching the end early is not a failure: a caller may seek past it, and
        /// the honest answer to a read there is nothing at all.
        bool discardTo(qint64 position)
        {
            QByteArray scratch(kReadBlockSize, Qt::Uninitialized);
            while (m_offset < position) {
                const qint64 wanted = std::min<qint64>(kReadBlockSize, position - m_offset);
                const qint64 read = readData(scratch.data(), wanted);
                if (read < 0)
                    return false;
                if (read == 0)
                    break;
            }
            return true;
        }

        QString m_archivePath;
        QString m_memberPath;
        qint64 m_knownSize = -1;
        /// Which header the member is, or -1 when the caller does not know and
        /// the name is all there is to go on.
        int m_ordinal = -1;
        OpenArchive m_open;
        /// Owned by m_open; held separately because it is what every read touches.
        ArchiveReader* m_reader = nullptr;
        /// Where the decompressed stream has actually reached, which is what a seek
        /// is measured against. QIODevice keeps its own idea of the position and
        /// the two are kept equal by seek().
        qint64 m_offset = 0;
        VfsError m_failure;
    };

} // namespace

ArchiveFileSystem::ArchiveFileSystem(QString archivePath)
    : m_archivePath(std::move(archivePath))
{
}

QString ArchiveFileSystem::authorityFor(const QString& archivePath)
{
    // The encoding itself is in core, where the uri is: two layers above this
    // decoded it by hand because they cannot see this file. See
    // core/vfs/PathWords.h and MOLE-403.
    return authorityFromLocalPath(archivePath);
}

QString ArchiveFileSystem::archivePathFromAuthority(const QString& authority)
{
    return localPathFromAuthority(authority);
}

VfsCapabilities ArchiveFileSystem::capabilities() const
{
    // Symlink without Write: a member that is a link can be read as one, and
    // nothing in an archive can be made. A copy asks the *destination* whether it
    // holds links, so advertising it here only says that readLink() answers.
    return VfsCapability::Read | VfsCapability::RandomAccessRead | VfsCapability::Symlink;
}

Result<void> ArchiveFileSystem::ensureIndexed()
{
    // Caller holds m_mutex.
    if (m_indexed)
        return {};

    if (!QFileInfo::exists(m_archivePath)) {
        return Result<void>::failure(
            VfsError::NotFound, QStringLiteral("Archive not found: %1").arg(m_archivePath));
    }

    OpenArchive open = openArchive(m_archivePath);
    ArchiveReader& reader = *open.reader;
    if (!reader.isOpen()) {
        return Result<void>::failure(VfsError::IoError,
            QStringLiteral("Cannot open archive %1: %2").arg(m_archivePath, reader.errorText()));
    }

    Node root;
    root.isDir = true;
    root.modified = QFileInfo(m_archivePath).lastModified();
    m_nodes.insert(QStringLiteral("/"), root);

    int ordinal = 0;
    while (struct archive_entry* entry = reader.nextHeader()) {
        const int at = ordinal++;
        const QString path = pathOfEntry(entry, m_archivePath, open.singleStream, reader.isZip());
        if (path.isEmpty())
            continue;

        Node node;
        node.ordinal = at;
        const auto filetype = archive_entry_filetype(entry);
        node.isDir = filetype == AE_IFDIR;
        node.size = node.isDir ? 0 : static_cast<qint64>(archive_entry_size(entry));

        // What the entry is, rather than "a directory or a file". A tarball of
        // almost any source release has links in it, and node_modules is mostly
        // links; each one used to arrive as a file whose bytes were empty, and a
        // copy out of the archive wrote empty files under the link names and
        // reported success. See MOLE-352.
        node.isSymlink = filetype == AE_IFLNK;
        if (node.isSymlink) {
            node.linkTarget = nameFromHeader(archive_entry_symlink_utf8(entry), archive_entry_symlink(entry));
        }
        // A hard link carries no data of its own: the bytes are in the member it
        // names, and reading the entry itself gives nothing at all.
        if (const QString hard
            = nameFromHeader(archive_entry_hardlink_utf8(entry), archive_entry_hardlink(entry));
            !hard.isEmpty()) {
            node.hardLinkTo = normaliseEntryPath(hard, reader.isZip());
        }
        switch (filetype) {
        case AE_IFIFO:
            node.special = SpecialKind::Pipe;
            break;
        case AE_IFSOCK:
            node.special = SpecialKind::Socket;
            break;
        case AE_IFBLK:
        case AE_IFCHR:
            node.special = SpecialKind::Device;
            break;
        default:
            break;
        }
        // A compressed stream does not know its uncompressed length until it has
        // been read, and `raw` says so by leaving the size unset. Unknown rather
        // than nought: a listing claiming a member with contents is 0 bytes is a
        // listing that lies, and a negative size is already how the models say
        // "no answer". gzip's trailer carries the length modulo 2^32, which is a
        // wrong answer above 4 GB rather than a slow one, so it is not used.
        if (!node.isDir && !archive_entry_size_is_set(entry))
            node.size = -1;
        if (archive_entry_mtime_is_set(entry))
            node.modified = QDateTime::fromSecsSinceEpoch(archive_entry_mtime(entry));
        m_nodes.insert(path, node);

        // Many archives omit directory records entirely, so synthesise every
        // parent or the tree would have holes in it.
        int slash = path.lastIndexOf(QLatin1Char('/'));
        while (slash > 0) {
            const QString parent = path.left(slash);
            if (m_nodes.contains(parent))
                break;
            Node made;
            made.isDir = true;
            made.modified = node.modified;
            m_nodes.insert(parent, made);
            slash = parent.lastIndexOf(QLatin1Char('/'));
        }

        archive_read_data_skip(reader.handle());
    }

    // Read as far as the archive goes, or as far as it could be read?
    //
    // Two things arrive here as "no more headers". One is the end of the
    // archive; the other is a container a format bidder accepted and then could
    // not parse -- the mtree bidder claiming a gzipped CSV is the case
    // openArchive()'s own comment names -- or a tarball whose end is missing.
    // Both used to be indexed as a complete archive: the first as a drive
    // holding nothing but `/`, the second as however much of the tree came
    // before the damage, with m_indexed recorded as if the whole thing had been
    // read. A short listing is what a mirror deletes against. See MOLE-352.
    if (!reader.endedAtTheEnd()) {
        m_nodes.clear();
        return Result<void>::failure(VfsError::IoError,
            QStringLiteral("Cannot read archive %1: %2").arg(m_archivePath, reader.errorText()));
    }

    // The children of each directory, worked out once. list() used to scan every
    // key in the index for a prefix, so walking a kernel tarball -- eighty
    // thousand entries, as many directories -- was some hundreds of millions of
    // string comparisons, all of them under the mutex.
    for (auto it = m_nodes.constBegin(); it != m_nodes.constEnd(); ++it) {
        const QString& path = it.key();
        if (path == QLatin1String("/"))
            continue;
        const int slash = path.lastIndexOf(QLatin1Char('/'));
        const QString parent = slash > 0 ? path.left(slash) : QStringLiteral("/");
        m_children[parent].append(path);
    }

    m_indexed = true;
    return {};
}

Result<FileEntryList> ArchiveFileSystem::list(const VfsUri& dir, const CancelToken& cancel)
{
    QMutexLocker lock(&m_mutex);
    if (Result<void> indexed = ensureIndexed(); !indexed.ok())
        return indexed.error();
    if (cancel.isCancelled())
        return VfsError::make(VfsError::Cancelled, QStringLiteral("Listing cancelled"));

    const QString path = dir.path();
    const auto node = m_nodes.constFind(path);
    if (node == m_nodes.constEnd())
        return VfsError::make(VfsError::NotFound, QStringLiteral("No such directory: %1").arg(path));
    if (!node->isDir)
        return VfsError::make(VfsError::NotADirectory, QStringLiteral("Not a directory: %1").arg(path));

    const QString prefix = (path == QLatin1String("/")) ? QStringLiteral("/") : path + QLatin1Char('/');

    FileEntryList out;
    for (const QString& candidate : m_children.value(path)) {
        const auto child = m_nodes.constFind(candidate);
        if (child == m_nodes.constEnd())
            continue;
        out.append(
            entryFor(VfsUri(scheme(), dir.authority(), candidate), candidate.mid(prefix.size()), *child));
    }

    std::sort(out.begin(), out.end(), [](const FileEntry& a, const FileEntry& b) { return a.name < b.name; });
    return out;
}

Result<FileEntry> ArchiveFileSystem::stat(const VfsUri& target)
{
    QMutexLocker lock(&m_mutex);
    if (Result<void> indexed = ensureIndexed(); !indexed.ok())
        return indexed.error();

    const auto node = m_nodes.constFind(target.path());
    if (node == m_nodes.constEnd())
        return VfsError::make(
            VfsError::NotFound, QStringLiteral("No such entry in archive: %1").arg(target.path()));

    return entryFor(target, target.fileName(), *node);
}

Result<QString> ArchiveFileSystem::readLink(const VfsUri& link)
{
    QMutexLocker lock(&m_mutex);
    if (Result<void> indexed = ensureIndexed(); !indexed.ok())
        return indexed.error();

    // Keyed the way stat() keys it: the paths in m_nodes are already normalised.
    const auto node = m_nodes.constFind(link.path());
    if (node == m_nodes.constEnd())
        return VfsError::make(
            VfsError::NotFound, QStringLiteral("No such entry in archive: %1").arg(link.path()));
    if (!node->isSymlink || node->linkTarget.isEmpty()) {
        return VfsError::make(VfsError::NotALink, QStringLiteral("Not a symbolic link: %1").arg(link.path()));
    }
    // The stored text, not resolvedLinkTarget(): what is copied out is what the
    // archive says, and resolving it against the member's own directory would
    // turn a relative link into one pinned to where it was extracted.
    return node->linkTarget;
}

FileEntry ArchiveFileSystem::entryFor(const VfsUri& uri, const QString& name, const Node& node) const
{
    // Caller holds m_mutex.
    FileEntry entry;
    entry.name = name;
    entry.uri = uri;
    entry.isDir = node.isDir;
    entry.isSymlink = node.isSymlink;
    entry.special = node.special;
    entry.isHidden = looksHidden(name);
    entry.isWritable = false;
    entry.size = node.size;
    entry.modified = node.modified;

    // A link whose target is not in this archive. Named rather than left as an
    // ordinary link, because the refusal above this layer has to say why -- see
    // SpecialKind and MOLE-333.
    if (node.isSymlink && entry.special == SpecialKind::None
        && resolvedLinkTarget(uri.path(), node.linkTarget).isEmpty()) {
        entry.special = SpecialKind::DanglingLink;
    }
    // A hard link is the member it names, so it is that member's size that a
    // reader will get and that a copy has to plan for.
    if (!node.hardLinkTo.isEmpty()) {
        const auto pointedAt = m_nodes.constFind(node.hardLinkTo);
        if (pointedAt != m_nodes.constEnd())
            entry.size = pointedAt->size;
        else
            entry.special = SpecialKind::DanglingLink;
    }
    return entry;
}

QString ArchiveFileSystem::resolvedLinkTarget(const QString& from, const QString& target) const
{
    // Caller holds m_mutex.
    if (target.isEmpty())
        return {};
    // Relative to the link's own directory, the way a link is read anywhere
    // else, and absolute targets resolve against the archive root -- there is
    // nothing else here for them to mean.
    const int slash = from.lastIndexOf(QLatin1Char('/'));
    const QString directory = slash > 0 ? from.left(slash) : QString();
    const QString joined
        = target.startsWith(QLatin1Char('/')) ? target : directory + QLatin1Char('/') + target;
    const QString resolved = normaliseEntryPath(joined, false);
    return m_nodes.contains(resolved) ? resolved : QString();
}

Result<std::unique_ptr<QIODevice>> ArchiveFileSystem::openRead(
    const VfsUri& target, qint64, const CancelToken& cancel)
{
    if (cancel.isCancelled()) {
        return Result<std::unique_ptr<QIODevice>>::failure(VfsError::Cancelled, QStringLiteral("Cancelled"));
    }
    // What the index knows about the member, which is what the device reports as
    // its size. The hint the caller passed is deliberately not used: IFileSystem
    // says expectedSize "is a hint about how to fetch, never a limit on what is
    // returned", and nothing here needs to break that -- a lazy device gives a
    // window without being told how big to make it.
    qint64 known = -1;
    int ordinal = -1;
    QString member = target.path();
    {
        QMutexLocker lock(&m_mutex);
        if (Result<void> indexed = ensureIndexed(); !indexed.ok())
            return indexed.error();

        auto node = m_nodes.constFind(target.path());
        if (node == m_nodes.constEnd())
            return VfsError::make(
                VfsError::NotFound, QStringLiteral("No such entry in archive: %1").arg(target.path()));
        if (node->isDir)
            return VfsError::make(
                VfsError::IsADirectory, QStringLiteral("Is a directory: %1").arg(target.path()));

        // A hard link's bytes are in the member it names -- the entry itself
        // carries none, so reading it used to hand back a zero-byte file. Read
        // that member instead, which is what "the same file under two names"
        // means inside a tarball.
        if (!node->hardLinkTo.isEmpty()) {
            const auto pointedAt = m_nodes.constFind(node->hardLinkTo);
            if (pointedAt == m_nodes.constEnd()) {
                return VfsError::make(VfsError::NotFound,
                    QStringLiteral("%1 is a link to %2, which is not in this archive")
                        .arg(target.path(), node->hardLinkTo));
            }
            member = node->hardLinkTo;
            node = pointedAt;
        }
        known = node->size;
        ordinal = node->ordinal;
    }

    // Stream formats have no random access, so reaching a byte means decompressing
    // everything before it. That is a cost in time, and it must not be one in
    // memory as well -- which is what holding the whole member made it. The device
    // decompresses as it is read and holds only the chunk it is handing over, so a
    // preview costs its window and an extraction that reads to the end still gets
    // every byte. See MOLE-218.
    auto device = std::make_unique<ArchiveMemberDevice>(m_archivePath, member, known, ordinal);
    if (!device->open(QIODevice::ReadOnly)) {
        // Every refusal inside the device records why. A Result built from an
        // error that is not one would be a success carrying no device at all,
        // which is a null the caller has no reason to expect.
        const VfsError why = device->failure();
        return why.isError()
            ? why
            : VfsError::make(VfsError::IoError, QStringLiteral("Cannot read %1").arg(target.path()));
    }
    return Result<std::unique_ptr<QIODevice>>(std::move(device));
}

QList<ConnectionField> ArchiveFileSystemFactory::connectionFields() const
{
    ConnectionField path;
    path.key = QStringLiteral("path");
    path.label = QStringLiteral("Archive file");
    path.kind = ConnectionField::Text;
    path.required = true;
    return { path };
}

FileSystemPtr ArchiveFileSystemFactory::create(const QVariantMap& config, QString* errorOut)
{
    const QString path = config.value(QStringLiteral("path")).toString();
    if (path.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("An archive mount needs a 'path'");
        return nullptr;
    }
    if (!QFileInfo::exists(path)) {
        if (errorOut)
            *errorOut = QStringLiteral("No such archive: %1").arg(path);
        return nullptr;
    }
    return std::make_shared<ArchiveFileSystem>(path);
}

QVariantMap ArchiveFileSystemFactory::configForFile(const QString& localPath) const
{
    return { { QStringLiteral("path"), localPath },
        { QStringLiteral("authority"), ArchiveFileSystem::authorityFor(localPath) },
        { QStringLiteral("rootPath"), QStringLiteral("/") } };
}

VfsUri ArchiveFileSystemFactory::rootUriForFile(const QString& localPath) const
{
    return VfsUri(QStringLiteral("archive"), ArchiveFileSystem::authorityFor(localPath), QStringLiteral("/"));
}

QVariantMap ArchiveFileSystemFactory::configForRoot(const VfsUri& root) const
{
    // The authority *is* the path, percent-encoded, which is what makes an
    // archive mount disposable: anything holding an `archive://` uri -- a
    // bookmark, a back step, a session restored tomorrow -- carries everything
    // needed to build the mount again. Where the file has gone, create() fails
    // the ordinary way for a file that is not there.
    if (root.scheme() != QLatin1String("archive"))
        return {};
    const QString path = ArchiveFileSystem::archivePathFromAuthority(root.authority());
    return path.isEmpty() ? QVariantMap {} : configForFile(path);
}

QStringList ArchiveFileSystemFactory::supportedSuffixes()
{
    // libarchive handles more than this; the list is what we advertise as
    // "double-click to open as a drive".
    //
    // **What earns a place: a container libarchive can read, whose primary
    // identity is a bundle of files rather than a document.** Both halves do
    // work. The first keeps off anything this build cannot actually open, because
    // a file that is offered and then fails is worse than one that was never
    // offered. The second is the interesting one, and it is a decision rather
    // than an oversight: .docx, .xlsx, .pptx, .odt, .ods, .odp and .epub are all
    // zips, and Mole previews every one of them properly. Making "open as a
    // folder" the default for a Word document would replace the thing a reader
    // wants with the thing they almost never want. Reaching inside one is worth
    // having as an explicit action beside the ordinary open; it is not this list.
    //
    // So the next format is judged rather than added by resemblance to one that
    // is already here. See MOLE-301.
    return { // Containers by name, and what libarchive has always read.
        QStringLiteral("zip"), QStringLiteral("tar"), QStringLiteral("gz"), QStringLiteral("tgz"),
        QStringLiteral("bz2"), QStringLiteral("xz"), QStringLiteral("zst"), QStringLiteral("7z"),
        QStringLiteral("rar"), QStringLiteral("iso"), QStringLiteral("cpio"), QStringLiteral("ar"),
        // A zip whose name says what the bundle is for. Every one of these is
        // read by the zip reader that was already here -- libarchive goes by what
        // is in the file, so the only thing that was missing was the name.
        QStringLiteral("jar"), QStringLiteral("war"), QStringLiteral("ear"), QStringLiteral("apk"),
        QStringLiteral("whl"), QStringLiteral("egg"), QStringLiteral("nupkg"), QStringLiteral("xpi"),
        QStringLiteral("vsix"),
        // Packages, which are containers with a header in front. A .deb is an ar
        // archive of three members -- so it opens onto debian-binary,
        // control.tar.* and data.tar.*, and the payload is one level further in
        // than somebody may expect. An .rpm is a cpio stream behind libarchive's
        // rpm filter.
        QStringLiteral("deb"), QStringLiteral("rpm")
    };
}

bool ArchiveFileSystemFactory::looksLikeArchive(const QString& fileName)
{
    const QString lower = fileName.toLower();
    const QStringList suffixes = supportedSuffixes();
    return std::any_of(suffixes.begin(), suffixes.end(),
        [&lower](const QString& suffix) { return lower.endsWith(QLatin1Char('.') + suffix); });
}

} // namespace mole
