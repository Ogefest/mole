#pragma once

#include "core/vfs/FileEntry.h"

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>

namespace mole::net {

/// One <response> from a PROPFIND multistatus document.
struct WebdavEntry
{
    /// Path part of the href, percent-decoded. Servers answer with anything from
    /// "/dav/notes.txt" to a full "https://host/dav/notes.txt", so only the path
    /// is kept.
    QString path;
    bool isCollection = false;
    /// kUnknownSize when the server sent no getcontentlength, which RFC 4918
    /// allows and servers do for generated resources.
    qint64 size = kUnknownSize;
    QDateTime modified;
    /// Status from the propstat, 200 when the properties came back usable.
    int status = 0;
};

/// Parses a PROPFIND answer.
///
/// Namespace prefixes are deliberately ignored -- servers use `D:`, `d:`, `lp1:`
/// or none at all for the same elements, and a parser that matched on the prefix
/// would work against one server and silently return nothing for the next.
bool parseMultistatus(const QByteArray& xml, QList<WebdavEntry>* entries, QString* errorOut);

/// The last segment of a path, which is what a listing shows as a name.
QString nameFromPath(const QString& path);

} // namespace mole::net
