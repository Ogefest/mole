#pragma once

#include "sdk/IArchiver.h"

#include <QList>
#include <QObject>
#include <QString>

#include <memory>
#include <vector>

namespace mole {

/// Everything that can pack files into one file, and what each one can write.
///
/// **The shell asks this instead of knowing a plugin.** `AppController` used to
/// call statics on the archive plugin's own `CompressTask` from seven members
/// behind `#ifdef MOLE_HAVE_ARCHIVE`; now it asks what formats exist and hands a
/// request to whichever archiver claims the chosen one. Nothing in `src/ui`
/// names a plugin, and a second plugin adding a format is a plugin registering
/// another `IArchiver`. See ADR-0101 and MOLE-415.
///
/// Empty is an ordinary answer: a build without libarchive loads no archive
/// plugin, so nothing is registered here and the shell offers no compression --
/// which is what it did before, by being compiled differently.
class ArchiveRegistry : public QObject
{
    Q_OBJECT

public:
    explicit ArchiveRegistry(QObject* parent = nullptr);
    ~ArchiveRegistry() override;

    /// Takes ownership. Refuses an archiver that offers no format at all, and one
    /// whose formats are all already claimed: two writers of `.zip` would make
    /// "which one" a question with no answer.
    bool addArchiver(std::unique_ptr<IArchiver> archiver);

    /// Every format anyone can write, in registration order. The first is the
    /// default the dialog opens on.
    QList<IArchiver::Format> formats() const;
    /// What `id` looks like, or a format with an empty id when nothing writes it.
    IArchiver::Format format(const QString& id) const;
    /// Whether anything at all can pack files here.
    bool canCompress() const { return !m_archivers.empty(); }

    /// Hands `request` to whichever archiver claims its format. False when
    /// nothing does, which the caller reports rather than this.
    bool compress(const IArchiver::Request& request);

private:
    IArchiver* archiverFor(const QString& formatId) const;

    std::vector<std::unique_ptr<IArchiver>> m_archivers;
};

} // namespace mole
