#pragma once

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

class QQuickTextDocument;

namespace mole {

/// Colours source code in a QML TextArea.
///
/// Deliberately lexical rather than a real parser: a preview must colour a
/// half-written file, a truncated one, or the middle of a 100 GB log that
/// starts mid-expression. A parser that refuses invalid input would leave
/// exactly those files plain, which is when colour helps most.
///
/// Languages are table-driven -- keywords, what starts a comment, which quotes
/// make a string. Adding one is a row in `rulesFor()`, not new code.
class SourceHighlighter final : public QSyntaxHighlighter
{
    Q_OBJECT

public:
    /// What makes a comment, a string and a keyword in one language.
    struct Rules
    {
        QString id;
        QString displayName;
        QStringList keywords;
        /// Types and built-in constants, coloured apart from keywords.
        QStringList builtins;
        QString lineComment;
        /// A few languages have two ways to start a line comment (SQL, Lua).
        QString altLineComment;
        QString blockCommentStart;
        QString blockCommentEnd;
        bool doubleQuoted = true;
        bool singleQuoted = true;
        bool backQuoted = false;
        /// XML and friends need the tag lexer, not the keyword one.
        bool markup = false;
        /// `"key": value` colouring, for JSON and YAML.
        bool keyValue = false;
    };

    explicit SourceHighlighter(QObject* parent = nullptr);

    /// The language id for a file suffix, or an empty string when nothing
    /// sensible applies. An unknown suffix means no colour rather than a wrong
    /// guess -- miscoloured text reads as a broken file.
    static QString languageForSuffix(const QString& suffix);

    /// The language for one file, from everything known about it: what it is
    /// called, what the content pass found it to be, and what it ends in.
    ///
    /// The order is name, then type, then suffix, and it is not the obvious one
    /// because for the files this exists for **the suffix is not where the
    /// information is**. `Dockerfile` and `Makefile` have no suffix at all,
    /// `CMakeLists.txt` has one that only says "text", and `.bashrc` has one
    /// VfsUri will not admit to. What each of them has is a name every
    /// programmer recognises, so the name is asked first; the sniffed type
    /// second, which is how a shell script called `deploy` gets coloured; and
    /// the suffix last, where it remains right for everything else.
    static QString languageFor(const QString& fileName, const QString& mimeType, const QString& suffix);

    /// The language for a file name that is a convention rather than a suffix.
    /// Exact names and one-step patterns (`Dockerfile.build`), no globbing:
    /// these are a short list of names people agreed on, not a pattern language.
    static QString languageForName(const QString& fileName);

    /// The language for a MIME type, for the few where the database is more
    /// specific than a suffix could be.
    static QString languageForMimeType(const QString& mimeType);
    static bool isSupported(const QString& suffix) { return !languageForSuffix(suffix).isEmpty(); }
    /// Every language this build can colour, for the documentation and tests.
    static QStringList supportedLanguages();
    /// Every file suffix that maps to a language, so the text viewer can
    /// accept exactly what can be coloured without keeping a second list.
    static QStringList knownSuffixes();
    static const Rules* rulesFor(const QString& languageId);

    void setLanguage(const QString& languageId);
    QString language() const { return m_languageId; }

    /// Attaches to the document behind a QML TextArea. Passing nullptr detaches.
    void attachTo(QQuickTextDocument* document);

protected:
    void highlightBlock(const QString& text) override;

private:
    void highlightCode(const QString& text, const Rules& rules);
    void highlightMarkup(const QString& text);
    /// Colours a run of digits starting at `start`; returns where it ended.
    int highlightNumber(const QString& text, int start);
    /// Colours a quoted run; returns where it ended, or text.size() when the
    /// string is left open at the end of the line.
    int highlightString(const QString& text, int start, QChar quote);

    QString m_languageId;
    const Rules* m_rules = nullptr;

    QTextCharFormat m_key;
    QTextCharFormat m_string;
    QTextCharFormat m_number;
    QTextCharFormat m_keyword;
    QTextCharFormat m_builtin;
    QTextCharFormat m_tag;
    QTextCharFormat m_attribute;
    QTextCharFormat m_comment;
    QTextCharFormat m_preprocessor;
};

} // namespace mole
