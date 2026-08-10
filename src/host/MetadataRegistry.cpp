#include "host/MetadataRegistry.h"

#include <algorithm>

namespace mole {

MetadataRegistry::MetadataRegistry(QObject* parent)
    : QObject(parent)
{
}

MetadataRegistry::~MetadataRegistry() = default;

bool MetadataRegistry::addReader(std::unique_ptr<IMetadataReader> reader)
{
    if (!reader || reader->id().isEmpty())
        return false;
    if (this->reader(reader->id()))
        return false;

    m_readers.push_back(std::move(reader));
    std::stable_sort(m_readers.begin(), m_readers.end(),
        [](const std::unique_ptr<IMetadataReader>& a, const std::unique_ptr<IMetadataReader>& b) {
            return a->priority() > b->priority();
        });
    return true;
}

QList<IMetadataReader*> MetadataRegistry::readersFor(const FileEntry& entry) const
{
    QList<IMetadataReader*> out;
    if (entry.isDir)
        return out;
    for (const auto& reader : m_readers) {
        if (reader->canRead(entry))
            out.append(reader.get());
    }
    return out;
}

IMetadataReader* MetadataRegistry::reader(const QString& id) const
{
    for (const auto& reader : m_readers) {
        if (reader->id() == id)
            return reader.get();
    }
    return nullptr;
}

QList<IMetadataReader*> MetadataRegistry::readers() const
{
    QList<IMetadataReader*> out;
    out.reserve(static_cast<qsizetype>(m_readers.size()));
    for (const auto& reader : m_readers)
        out.append(reader.get());
    return out;
}

} // namespace mole
