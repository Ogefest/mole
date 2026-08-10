#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QHash>
#include <QStringList>

namespace mole {

/// Pulls named members out of the *front* of a zip, without the rest of it.
///
/// A `.docx` is a zip whose author's name is a few hundred bytes of XML, and a
/// zip's local headers appear in stream order -- so the members near the front
/// can be read from a prefix, with no central directory and no seek to the end
/// of the file. That is what makes reading a 100 MB document's properties cost
/// a few hundred kilobytes.
///
/// The prefix is walked with libarchive over memory, so a truncated container is
/// its ordinary end-of-data case rather than something this has to detect: what
/// is whole is returned and the rest is simply not there.
///
/// Only the members asked for are kept, and each is capped, because a member
/// inside an archive declares its own size and a declared size is a claim --
/// the same lesson as ADR-0010's entry paths.
QHash<QString, QByteArray> membersFromZipPrefix(
    QByteArrayView prefix, const QStringList& wanted, qint64 maxMemberBytes = 1024 * 1024);

} // namespace mole
