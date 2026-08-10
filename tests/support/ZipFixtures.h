#pragma once

#include <QByteArray>
#include <QList>

#include <array>

namespace mole::test {

/// A zip built by hand, stored rather than deflated, so a test can say exactly
/// what is in a container and in what order.
///
/// Stored entries because a test about *where the members are* has no business
/// depending on how well its XML compresses -- and because a reader walking a
/// prefix must work on the plainest zip anybody writes.
class StoredZip
{
public:
    void add(const QByteArray& name, const QByteArray& contents)
    {
        Member member;
        member.name = name;
        member.contents = contents;
        member.offset = m_local.size();
        m_local += localHeader(member);
        m_local += name;
        m_local += contents;
        m_members.append(member);
    }

    /// Bytes of padding before the next member, as a comment field on a stored
    /// entry -- the way a real container ends up with its properties a long way
    /// in.
    void addFiller(qsizetype bytes)
    {
        add(QByteArrayLiteral("filler/") + QByteArray::number(m_members.size()), QByteArray(bytes, 'x'));
    }

    QByteArray build() const
    {
        QByteArray out = m_local;
        const qint64 directoryAt = out.size();
        for (const Member& member : m_members)
            out += centralHeader(member);
        const qint64 directoryBytes = out.size() - directoryAt;

        out += QByteArrayLiteral("PK\x05\x06");
        out += u16(0) + u16(0);
        out += u16(quint16(m_members.size())) + u16(quint16(m_members.size()));
        out += u32(quint32(directoryBytes)) + u32(quint32(directoryAt));
        out += u16(0);
        return out;
    }

private:
    struct Member
    {
        QByteArray name;
        QByteArray contents;
        qint64 offset = 0;
    };

    static QByteArray u16(quint16 value)
    {
        QByteArray out(2, '\0');
        out[0] = char(value & 0xff);
        out[1] = char((value >> 8) & 0xff);
        return out;
    }

    static QByteArray u32(quint32 value)
    {
        QByteArray out(4, '\0');
        for (int i = 0; i < 4; ++i)
            out[i] = char((value >> (8 * i)) & 0xff);
        return out;
    }

    static quint32 crc32Of(const QByteArray& data)
    {
        static const auto table = [] {
            std::array<quint32, 256> made {};
            for (quint32 i = 0; i < 256; ++i) {
                quint32 c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
                made[i] = c;
            }
            return made;
        }();

        quint32 crc = 0xffffffffu;
        for (const char byte : data)
            crc = table[(crc ^ static_cast<unsigned char>(byte)) & 0xff] ^ (crc >> 8);
        return crc ^ 0xffffffffu;
    }

    static QByteArray localHeader(const Member& member)
    {
        QByteArray out = QByteArrayLiteral("PK\x03\x04");
        out += u16(20) + u16(0) + u16(0); // version, flags, stored
        out += u16(0) + u16(0); // time, date
        out += u32(crc32Of(member.contents));
        out += u32(quint32(member.contents.size())) + u32(quint32(member.contents.size()));
        out += u16(quint16(member.name.size())) + u16(0);
        return out;
    }

    static QByteArray centralHeader(const Member& member)
    {
        QByteArray out = QByteArrayLiteral("PK\x01\x02");
        out += u16(20) + u16(20) + u16(0) + u16(0);
        out += u16(0) + u16(0);
        out += u32(crc32Of(member.contents));
        out += u32(quint32(member.contents.size())) + u32(quint32(member.contents.size()));
        out += u16(quint16(member.name.size())) + u16(0) + u16(0);
        out += u16(0) + u16(0) + u32(0);
        out += u32(quint32(member.offset));
        out += member.name;
        return out;
    }

    QByteArray m_local;
    QList<Member> m_members;
};

} // namespace mole::test
