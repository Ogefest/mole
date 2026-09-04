#pragma once

#include <QByteArrayView>
#include <QString>
#include <QStringConverter>

namespace mole {

/// What a file is, from its name and the first page of it.
///
/// The preview layer's second question. `IPreviewProvider::canPreview()` may
/// only look at a name, which is why a `Dockerfile` used to be nine facts out of
/// `stat()`: shared-mime-info has no glob for that name, so the name-only answer
/// is `application/octet-stream` and no viewer claims it.
///
/// Nothing here reads a file. The caller hands over a sample it has already
/// read -- through ReadRangeTask, on a worker thread -- so a 100 GB mbox with no
/// extension is identified as fast as a 100 byte one.
class FileType
{
public:
    /// The most bytes anything needs to identify a file. One page: every magic
    /// rule in shared-mime-info matches within the first few hundred bytes, and
    /// the text test below is a sample rather than a survey.
    static constexpr qint64 kSampleBytes = 4096;

    /// The MIME type of a file called `name` whose contents start with `head`.
    /// Never empty.
    ///
    /// Both answers come from the installed shared-mime-info database, asked
    /// twice: once for what the bytes say (the magic rules) and once for what the
    /// name says (the globs). Deciding between the two is this function's whole
    /// job, because Qt's combined lookup cannot do it --
    /// `QMimeDatabase::mimeTypeForFileNameAndData()` returns the glob match
    /// whenever exactly one glob matches and never looks at the bytes at all, so
    /// a zip renamed `notes.txt` comes back from Qt as `text/plain`. The magic
    /// table itself is never reimplemented here; only the choice between two of
    /// the database's own answers. See
    /// docs/adr/0033-a-file-is-identified-by-its-contents.md.
    ///
    /// The rules, in order:
    /// - A magic match beats the name, because the bytes are the file and the
    ///   name is a label somebody typed -- unless the name's answer is a
    ///   *subclass* of it, which is the two agreeing and the more specific of
    ///   the two. A `.docx` is a zip, and answering "zip" would throw away what
    ///   the name knew.
    /// - When both say text, the name wins. Magic for text formats is thin --
    ///   an `#include` makes any C++ file C -- and a wrong language colours a
    ///   file oddly, where a wrong viewer shows the wrong thing entirely.
    /// - When no magic rule matches, the bytes still answer one question: text or
    ///   not. A name claiming text over binary bytes loses it, and so does a name
    ///   claiming a binary format over text; a name that says nothing leaves
    ///   `text/plain` or `application/octet-stream`.
    static QString identify(const QString& name, QByteArrayView head);

    /// Whether a sample reads as text. Used for the case Qt's own heuristic gives
    /// up on and answers `application/octet-stream`.
    ///
    /// - A UTF-16 or UTF-8 byte order mark means text whatever follows it: a
    ///   UTF-16 file is half NUL bytes, and every other rule here would call it
    ///   binary.
    /// - A NUL byte anywhere in the sample means binary. No text encoding without
    ///   a BOM contains one, and practically every binary format does.
    /// - Otherwise the C0 control characters are counted -- anything below 0x20
    ///   that is not tab, newline, carriage return or form feed. More than 2% of
    ///   the sample means binary. 2% rather than none, because a log with a stray
    ///   ESC or BEL in it is still a log; 2% rather than more, because binary
    ///   formats are dense with them -- a page of ELF or of a compressed stream
    ///   runs to a tenth control bytes or worse.
    /// - What is left is decoded as UTF-8. Clean UTF-8 is text. Bytes that are
    ///   not UTF-8 are judged as Latin-1 instead, where 0x80..0x9F is control
    ///   characters too and the same 2% applies -- a Latin-1 log is text, and
    ///   random bytes are a fifth C1 controls.
    ///
    /// A sample cut mid-character is not binary: it is decoded as one chunk, and
    /// a truncated sequence at the end of a chunk leaves the decoder waiting for
    /// the rest rather than failing.
    ///
    /// An empty sample is text. Nothing in an empty file is binary, and an empty
    /// text viewer says more about it than a list of properties does.
    static bool looksLikeText(QByteArrayView sample);

    /// Which encoding to read a text file with, from the same sample.
    ///
    /// **The decision looksLikeText() already makes, said out loud**, because two
    /// places were making it separately and disagreeing: the sniffer admits a
    /// Latin-1 log as text and the content search then decoded it as UTF-8 and
    /// reported it as undecodable -- so a file the search offered to look inside
    /// was skipped, and the two halves of the application disagreed about what
    /// text is. See MOLE-405.
    ///
    /// A byte order mark decides it outright. Failing that, bytes that decode as
    /// UTF-8 are UTF-8 and bytes that do not are Latin-1, which is the fallback
    /// that cannot fail -- every byte sequence is valid Latin-1. That is a guess
    /// and not a detection: a cp1252 or a KOI8-R file comes back as Latin-1,
    /// which is right about the ASCII range and wrong in the same places any
    /// single-byte guess is wrong. It is still much better than U+FFFD, which is
    /// what a UTF-8 decoder produces for every non-ASCII character in the file.
    static QStringConverter::Encoding encodingFor(QByteArrayView sample);

    /// Whether this name is a single compressed stream rather than a container:
    /// `.gz`, `.xz`, `.bz2` or `.zst`, and not `.tgz` or `.tar.gz`.
    ///
    /// A fact about the name and nothing else, which is why it lives here rather
    /// than in the archive backend. Two places need it and neither may depend on
    /// the other: the backend decides from it whether to retry an open with
    /// libarchive's `raw` format, which no container needs (MOLE-216), and the
    /// preview tab decides from it whether the file is a wrapper around the thing
    /// somebody actually wanted to look at (MOLE-219).
    static bool namesSingleCompressedStream(const QString& name);

    /// The share of a sample that may be control characters before it is taken
    /// for binary. Here so a test can state the threshold it is testing.
    static constexpr int kControlPercent = 2;
};

} // namespace mole
