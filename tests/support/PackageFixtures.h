#pragma once

#include <QByteArray>
#include <QList>
#include <QPair>

namespace mole::test {

/// A `.deb` and an `.rpm`, built out of bytes here rather than by a packaging
/// tool.
///
/// Both are containers whose identity is a bundle of files, which is what earns a
/// place on `ArchiveFileSystemFactory::supportedSuffixes()` -- and both had to be
/// checked rather than reasoned about, because a format that is recognised and
/// cannot be read is worse than one that was never offered. See MOLE-301.
///
/// **No `dpkg-deb` and no `rpmbuild`, because neither is on every machine that
/// runs this suite.** `dpkg-deb` is Debian's and absent on Fedora, where the tier
/// also runs; `rpmbuild` is the other way round. A fixture that exists only where
/// a tool happens to be installed is a check that quietly stops running, so these
/// are assembled by hand: an `ar` archive is a text header per member, and an rpm
/// is a 96-byte lead, two header sections, and a payload the rpm filter hands to
/// whatever reads it next.

/// The members of an `ar` archive, in order. This is what a `.deb` is: three of
/// them, called `debian-binary`, `control.tar.*` and `data.tar.*`.
inline QByteArray arArchive(const QList<QPair<QByteArray, QByteArray>>& members)
{
    auto field = [](const QByteArray& value, int width) { return value.leftJustified(width, ' '); };

    QByteArray out = QByteArrayLiteral("!<arch>\n");
    for (const auto& member : members) {
        out += field(member.first, 16); // name
        out += field(QByteArrayLiteral("0"), 12); // modified
        out += field(QByteArrayLiteral("0"), 6) + field(QByteArrayLiteral("0"), 6); // owner, group
        out += field(QByteArrayLiteral("100644"), 8); // mode
        out += field(QByteArray::number(member.second.size()), 10); // size
        out += QByteArrayLiteral("`\n"); // end of header
        out += member.second;
        // Members start on an even offset, and the padding is a newline rather
        // than a NUL so the file stays readable in a pager.
        if (member.second.size() % 2 != 0)
            out += '\n';
    }
    return out;
}

/// A cpio stream in the `newc` format -- the payload of an rpm.
///
/// One 110-byte header of hexadecimal fields per file, then the name, then the
/// contents, each padded to a multiple of four; and a member called `TRAILER!!!`
/// to say where it ends.
inline QByteArray cpioNewc(const QList<QPair<QByteArray, QByteArray>>& files)
{
    auto header = [](std::initializer_list<qint64> fields) {
        QByteArray out = QByteArrayLiteral("070701");
        for (qint64 value : fields)
            out += QByteArray::number(value, 16).toUpper().rightJustified(8, '0');
        return out;
    };
    auto pad = [](QByteArray& out) {
        while (out.size() % 4 != 0)
            out += '\0';
    };

    QByteArray out;
    qint64 inode = 1;
    for (const auto& file : files) {
        const QByteArray name = file.first + '\0';
        //           ino     mode     uid gid nlink mtime  size
        out += header({ inode++, 0100644, 0, 0, 1, 0, file.second.size(),
            //   devmajor devminor rdevmajor rdevminor namesize check
            0, 0, 0, 0, name.size(), 0 });
        out += name;
        pad(out);
        out += file.second;
        pad(out);
    }
    const QByteArray end = QByteArrayLiteral("TRAILER!!!") + '\0';
    out += header({ 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, end.size(), 0 });
    out += end;
    pad(out);
    return out;
}

/// An `.rpm`: the lead, the two header sections, and a cpio payload.
///
/// **The payload here is not compressed, and that is the one way this differs
/// from a package `rpmbuild` would write.** libarchive's rpm support is a
/// *filter*: it walks the lead and the headers, then hands what is left to
/// whatever reads next -- zstd on Fedora, gzip on an older package, and a plain
/// cpio reader here. So what this fixture asks is exactly the question the suffix
/// raised: is the rpm filter in this build of libarchive, and is the payload
/// behind it read? Which compressor sits between the two is asked directly by the
/// gzip, xz and bzip2 cases in the same suite, and a real zstd package was opened
/// by hand on both distributions before the suffix was added.
inline QByteArray rpmPackage(const QList<QPair<QByteArray, QByteArray>>& files)
{
    auto beInt16
        = [](quint16 value) { return QByteArray(1, char(value >> 8)) + QByteArray(1, char(value & 0xff)); };
    auto beInt32 = [](quint32 value) {
        QByteArray out;
        for (int shift = 24; shift >= 0; shift -= 8)
            out += char((value >> shift) & 0xff);
        return out;
    };

    // The lead: 96 bytes, and only its magic is still load-bearing. rpm itself
    // stopped believing the rest of it years ago, and so does the filter.
    QByteArray lead = QByteArrayLiteral("\xed\xab\xee\xdb");
    lead += char(3);
    lead += char(0); // version 3.0
    lead += beInt16(0); // type: a binary package
    lead += beInt16(1); // architecture
    lead += QByteArrayLiteral("mole-fixture-1-1").leftJustified(66, '\0');
    lead += beInt16(1); // operating system
    lead += beInt16(5); // signature type
    lead += QByteArray(16, '\0'); // reserved
    Q_ASSERT(lead.size() == 96);

    // A header section with nothing in it: the magic, four reserved bytes, no
    // index entries and no store. Two of them, because a package carries a
    // signature header and then its own.
    const QByteArray section
        = QByteArrayLiteral("\x8e\xad\xe8\x01") + QByteArray(4, '\0') + beInt32(0) + beInt32(0);
    QByteArray signature = section;
    while (signature.size() % 8 != 0) // the signature header is padded to eight
        signature += '\0';

    return lead + signature + section + cpioNewc(files);
}

} // namespace mole::test
