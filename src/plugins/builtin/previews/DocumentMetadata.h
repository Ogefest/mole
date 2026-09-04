#pragma once

#include "sdk/IMetadataReader.h"

namespace mole {

/// Who wrote a document, out of the front of the container it lives in.
///
/// `.docx`, `.xlsx` and `.pptx` are zips carrying `docProps/core.xml` and
/// `docProps/app.xml`; `.odt`, `.ods` and `.odp` are zips carrying one
/// `meta.xml`. One reader for both, because the difference between them is which
/// member to look in and which element names to expect.
///
/// **A bounded prefix, never the file.** A zip's local headers appear in stream
/// order and every writer we can find puts those members near the front, so
/// `kPrefixBytes` is enough to read them without the central directory at the
/// end. When they are not in the prefix the reader says so rather than fetching
/// the rest: a `.docx` can be 100 MB and an author's name is not worth 100 MB
/// over a network drive.
///
/// **Nothing the XML names is resolved.** No entities, no external DTD, no
/// schema. ADR-0006's rule that previewing a file puts nothing on the network
/// covers a document's properties too, and an entity pointing at a local file is
/// the same fault as an `<img>` pointing at a remote one.
///
/// Only in a build with libarchive. Without it the reader still exists, claims
/// nothing, and the generic facts are all a document gets.
class DocumentMetadataReader final : public IMetadataReader
{
public:
    QString id() const override { return QStringLiteral("mole.metadata.document"); }
    int priority() const override { return 100; }
    bool canRead(const FileEntry& entry) const override;
    QList<FileFact> read(const FileEntry& entry, QByteArrayView head, const PluginServices& services,
        const CancelToken& cancel) const override;

    /// How much of a container is ever read. A quarter of a megabyte holds the
    /// properties of every document anybody has produced for us, and bounds the
    /// cost of a reader nobody may even look at.
    static constexpr qint64 kPrefixBytes = 256 * 1024;

    /// The facts in a prefix of a container, with no I/O at all. Public because
    /// this is where the parsing risk lives and it deserves bytes a test wrote.
    static QList<FileFact> factsFor(QByteArrayView prefix);

    /// Whether this prefix is the front of a document container rather than of
    /// some other zip.
    ///
    /// The member every one of these formats writes first by specification:
    /// `[Content_Types].xml` or `mimetype`. See read() and MOLE-383.
    static bool looksLikeADocumentContainer(QByteArrayView prefix);

    /// True in a build that can open a container at all.
    static bool isAvailable();
};

} // namespace mole
