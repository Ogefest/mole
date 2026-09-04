#include "host/ArchiveRegistry.h"

namespace mole {

ArchiveRegistry::ArchiveRegistry(QObject* parent)
    : QObject(parent)
{
}

ArchiveRegistry::~ArchiveRegistry() = default;

bool ArchiveRegistry::addArchiver(std::unique_ptr<IArchiver> archiver)
{
    if (!archiver)
        return false;
    const QList<IArchiver::Format> offered = archiver->formats();
    if (offered.isEmpty())
        return false;

    // A format nobody else claims. Two archivers both writing `.zip` would leave
    // "which one packs this" to registration order, which is not an answer
    // anybody could predict or debug.
    for (const IArchiver::Format& format : offered) {
        if (format.id.isEmpty() || archiverFor(format.id))
            return false;
    }

    m_archivers.push_back(std::move(archiver));
    return true;
}

QList<IArchiver::Format> ArchiveRegistry::formats() const
{
    QList<IArchiver::Format> all;
    for (const std::unique_ptr<IArchiver>& archiver : m_archivers)
        all.append(archiver->formats());
    return all;
}

IArchiver::Format ArchiveRegistry::format(const QString& id) const
{
    for (const std::unique_ptr<IArchiver>& archiver : m_archivers) {
        for (const IArchiver::Format& format : archiver->formats()) {
            if (format.id == id)
                return format;
        }
    }
    return {};
}

bool ArchiveRegistry::compress(const IArchiver::Request& request)
{
    IArchiver* archiver = archiverFor(request.formatId);
    return archiver && archiver->compress(request);
}

IArchiver* ArchiveRegistry::archiverFor(const QString& formatId) const
{
    if (formatId.isEmpty())
        return nullptr;
    for (const std::unique_ptr<IArchiver>& archiver : m_archivers) {
        for (const IArchiver::Format& format : archiver->formats()) {
            if (format.id == formatId)
                return archiver.get();
        }
    }
    return nullptr;
}

} // namespace mole
