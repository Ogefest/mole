#include "plugins/builtin/previews/SyntaxHighlighter.h"

#include <QQuickTextDocument>
#include <QRegularExpression>

namespace mole {
namespace {

    // Tuned against the dark theme the shell uses.
    constexpr QLatin1String kKeyColour("#7cc4ff");
    constexpr QLatin1String kStringColour("#a5d6a7");
    constexpr QLatin1String kNumberColour("#f5b76b");
    constexpr QLatin1String kKeywordColour("#c792ea");
    constexpr QLatin1String kBuiltinColour("#89ddff");
    constexpr QLatin1String kTagColour("#7cc4ff");
    constexpr QLatin1String kAttributeColour("#f5b76b");
    constexpr QLatin1String kCommentColour("#6f7788");
    constexpr QLatin1String kPreprocessorColour("#e5947b");

    /// States carried across lines. A block comment that opens on line 4000 and
    /// closes on line 4200 has to survive the 4199 lines in between.
    enum BlockState { StateNormal = 0, StateInBlockComment = 1 };

    QStringList split(const char* words)
    {
        return QString::fromLatin1(words).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    }

    using Rules = SourceHighlighter::Rules;

    /// Every language this build knows, keyed by id.
    ///
    /// Built once. The lists are deliberately short: the point is to make
    /// structure visible at a glance, not to prove the table is exhaustive.
    const QHash<QString, Rules>& languageTable()
    {
        static const QHash<QString, Rules> table = [] {
            QHash<QString, Rules> out;

            const auto add = [&out](Rules rules) { out.insert(rules.id, rules); };

            Rules c;
            c.id = QStringLiteral("cpp");
            c.displayName = QStringLiteral("C++");
            c.keywords = split("alignas alignof and asm auto break case catch class concept const "
                               "consteval constexpr constinit const_cast continue co_await co_return "
                               "co_yield decltype default delete do dynamic_cast else enum explicit "
                               "export extern for friend goto if inline mutable namespace new noexcept "
                               "not operator or private protected public register reinterpret_cast "
                               "requires return sizeof static static_assert static_cast struct switch "
                               "template this thread_local throw try typedef typeid typename union "
                               "using virtual volatile while xor");
            c.builtins = split("bool char char8_t char16_t char32_t double false float int long "
                               "nullptr short signed size_t true unsigned void wchar_t int8_t int16_t "
                               "int32_t int64_t uint8_t uint16_t uint32_t uint64_t");
            c.lineComment = QStringLiteral("//");
            c.blockCommentStart = QStringLiteral("/*");
            c.blockCommentEnd = QStringLiteral("*/");
            add(c);

            Rules python;
            python.id = QStringLiteral("python");
            python.displayName = QStringLiteral("Python");
            python.keywords = split("and as assert async await break class continue def del elif else "
                                    "except finally for from global if import in is lambda nonlocal "
                                    "not or pass raise return try while with yield match case");
            python.builtins = split("True False None self bool bytes dict float int list object set "
                                    "str tuple len range print open enumerate zip super isinstance");
            python.lineComment = QStringLiteral("#");
            add(python);

            Rules js;
            js.id = QStringLiteral("javascript");
            js.displayName = QStringLiteral("JavaScript");
            js.keywords = split("as async await break case catch class const continue debugger default "
                                "delete do else export extends finally for from function get if import "
                                "in instanceof let new of return set static super switch this throw try "
                                "typeof var void while with yield interface type enum implements "
                                "declare namespace readonly public private protected abstract");
            js.builtins = split("true false null undefined NaN Infinity Array Boolean Date Error JSON "
                                "Math Number Object Promise RegExp String Symbol console window "
                                "document string number boolean any unknown never");
            js.lineComment = QStringLiteral("//");
            js.blockCommentStart = QStringLiteral("/*");
            js.blockCommentEnd = QStringLiteral("*/");
            js.backQuoted = true;
            add(js);

            Rules qml = js;
            qml.id = QStringLiteral("qml");
            qml.displayName = QStringLiteral("QML");
            qml.keywords += split("property signal readonly component required on pragma");
            add(qml);

            Rules java;
            java.id = QStringLiteral("java");
            java.displayName = QStringLiteral("Java");
            java.keywords = split("abstract assert break case catch class const continue default do "
                                  "else enum extends final finally for goto if implements import "
                                  "instanceof interface native new package private protected public "
                                  "return static strictfp super switch synchronized this throw throws "
                                  "transient try volatile while var record sealed permits yield");
            java.builtins = split("boolean byte char double float int long short void true false null "
                                  "String Object List Map Integer Double Boolean");
            java.lineComment = QStringLiteral("//");
            java.blockCommentStart = QStringLiteral("/*");
            java.blockCommentEnd = QStringLiteral("*/");
            add(java);

            Rules kotlin = java;
            kotlin.id = QStringLiteral("kotlin");
            kotlin.displayName = QStringLiteral("Kotlin");
            kotlin.keywords
                = split("as break by catch class companion const constructor continue crossinline "
                        "data do else enum external false final finally for fun get if import in "
                        "infix init inline interface internal is lateinit noinline null object "
                        "open operator out override package private protected public reified "
                        "return sealed set super suspend tailrec this throw true try typealias "
                        "val var vararg when where while");
            add(kotlin);

            Rules csharp;
            csharp.id = QStringLiteral("csharp");
            csharp.displayName = QStringLiteral("C#");
            csharp.keywords = split("abstract as async await base break case catch checked class const "
                                    "continue default delegate do else enum event explicit extern "
                                    "finally fixed for foreach goto if implicit in interface internal "
                                    "is lock namespace new operator out override params private "
                                    "protected public readonly ref return sealed sizeof stackalloc "
                                    "static struct switch this throw try typeof unchecked unsafe using "
                                    "virtual volatile while yield record var");
            csharp.builtins = split("bool byte char decimal double float int long object sbyte short "
                                    "string uint ulong ushort void true false null");
            csharp.lineComment = QStringLiteral("//");
            csharp.blockCommentStart = QStringLiteral("/*");
            csharp.blockCommentEnd = QStringLiteral("*/");
            add(csharp);

            Rules go;
            go.id = QStringLiteral("go");
            go.displayName = QStringLiteral("Go");
            go.keywords = split("break case chan const continue default defer else fallthrough for "
                                "func go goto if import interface map package range return select "
                                "struct switch type var");
            go.builtins = split("bool byte complex64 complex128 error float32 float64 int int8 int16 "
                                "int32 int64 rune string uint uint8 uint16 uint32 uint64 uintptr true "
                                "false nil iota append cap close copy delete len make new panic recover");
            go.lineComment = QStringLiteral("//");
            go.blockCommentStart = QStringLiteral("/*");
            go.blockCommentEnd = QStringLiteral("*/");
            go.backQuoted = true;
            add(go);

            Rules rust;
            rust.id = QStringLiteral("rust");
            rust.displayName = QStringLiteral("Rust");
            rust.keywords = split("as async await break const continue crate dyn else enum extern fn "
                                  "for if impl in let loop match mod move mut pub ref return self "
                                  "Self static struct super trait type unsafe use where while");
            rust.builtins = split("bool char f32 f64 i8 i16 i32 i64 i128 isize str u8 u16 u32 u64 "
                                  "u128 usize String Vec Option Result Some None Ok Err true false");
            rust.lineComment = QStringLiteral("//");
            rust.blockCommentStart = QStringLiteral("/*");
            rust.blockCommentEnd = QStringLiteral("*/");
            add(rust);

            Rules ruby;
            ruby.id = QStringLiteral("ruby");
            ruby.displayName = QStringLiteral("Ruby");
            ruby.keywords = split("alias and begin break case class def defined? do else elsif end "
                                  "ensure for if in module next not or redo rescue retry return self "
                                  "super then undef unless until when while yield require require_relative");
            ruby.builtins = split("true false nil puts print attr_accessor attr_reader attr_writer "
                                  "String Integer Float Array Hash Symbol");
            ruby.lineComment = QStringLiteral("#");
            add(ruby);

            Rules php;
            php.id = QStringLiteral("php");
            php.displayName = QStringLiteral("PHP");
            php.keywords = split("abstract and array as break callable case catch class clone const "
                                 "continue declare default do echo else elseif empty enddeclare endfor "
                                 "endforeach endif endswitch endwhile enum extends final finally fn for "
                                 "foreach function global if implements include include_once instanceof "
                                 "insteadof interface isset list match namespace new or print private "
                                 "protected public readonly require require_once return static switch "
                                 "throw trait try unset use var while xor yield");
            php.builtins = split("true false null int float string bool object mixed void self parent");
            php.lineComment = QStringLiteral("//");
            php.altLineComment = QStringLiteral("#");
            php.blockCommentStart = QStringLiteral("/*");
            php.blockCommentEnd = QStringLiteral("*/");
            add(php);

            Rules shell;
            shell.id = QStringLiteral("shell");
            shell.displayName = QStringLiteral("Shell");
            shell.keywords = split("if then else elif fi case esac for while until do done function "
                                   "in select time coproc return break continue local export readonly "
                                   "declare typeset unset shift source eval exec trap set");
            shell.builtins = split("echo printf cd pwd test true false read cat grep sed awk cut sort "
                                   "uniq head tail find xargs mkdir rm cp mv chmod chown ln");
            shell.lineComment = QStringLiteral("#");
            add(shell);

            Rules sql;
            sql.id = QStringLiteral("sql");
            sql.displayName = QStringLiteral("SQL");
            sql.keywords = split("ADD ALL ALTER AND AS ASC BEGIN BETWEEN BY CASE CAST COMMIT CREATE "
                                 "CROSS DELETE DESC DISTINCT DROP ELSE END EXCEPT EXISTS FROM FULL "
                                 "GROUP HAVING IN INDEX INNER INSERT INTERSECT INTO IS JOIN LEFT LIKE "
                                 "LIMIT NOT NULL OFFSET ON OR ORDER OUTER PRIMARY RIGHT ROLLBACK "
                                 "SELECT SET TABLE THEN UNION UNIQUE UPDATE USING VALUES VIEW WHEN "
                                 "WHERE WITH");
            sql.builtins = split("INT INTEGER BIGINT SMALLINT REAL DOUBLE DECIMAL NUMERIC CHAR VARCHAR "
                                 "TEXT BLOB DATE TIME TIMESTAMP BOOLEAN COUNT SUM AVG MIN MAX COALESCE");
            sql.lineComment = QStringLiteral("--");
            sql.blockCommentStart = QStringLiteral("/*");
            sql.blockCommentEnd = QStringLiteral("*/");
            add(sql);

            Rules yaml;
            yaml.id = QStringLiteral("yaml");
            yaml.displayName = QStringLiteral("YAML");
            yaml.builtins = split("true false null yes no on off ~");
            yaml.lineComment = QStringLiteral("#");
            yaml.keyValue = true;
            add(yaml);

            Rules ini;
            ini.id = QStringLiteral("ini");
            ini.displayName = QStringLiteral("INI / TOML");
            ini.builtins = split("true false");
            ini.lineComment = QStringLiteral("#");
            ini.altLineComment = QStringLiteral(";");
            ini.keyValue = true;
            add(ini);

            Rules cmake;
            cmake.id = QStringLiteral("cmake");
            cmake.displayName = QStringLiteral("CMake");
            cmake.keywords = split("if elseif else endif foreach endforeach while endwhile function "
                                   "endfunction macro endmacro return break continue set unset list "
                                   "string file find_package include option project add_executable "
                                   "add_library add_subdirectory target_link_libraries target_sources "
                                   "target_include_directories install message");
            cmake.builtins = split("ON OFF TRUE FALSE NOT AND OR STREQUAL MATCHES EXISTS REQUIRED "
                                   "PUBLIC PRIVATE INTERFACE STATIC SHARED");
            cmake.lineComment = QStringLiteral("#");
            add(cmake);

            Rules make;
            make.id = QStringLiteral("make");
            make.displayName = QStringLiteral("Makefile");
            make.keywords = split("ifeq ifneq ifdef ifndef else endif include export unexport "
                                  "override define endef vpath");
            make.lineComment = QStringLiteral("#");
            add(make);

            Rules docker;
            docker.id = QStringLiteral("dockerfile");
            docker.displayName = QStringLiteral("Dockerfile");
            docker.keywords = split("FROM RUN CMD LABEL MAINTAINER EXPOSE ENV ADD COPY ENTRYPOINT "
                                    "VOLUME USER WORKDIR ARG ONBUILD STOPSIGNAL HEALTHCHECK SHELL AS");
            docker.lineComment = QStringLiteral("#");
            add(docker);

            Rules lua;
            lua.id = QStringLiteral("lua");
            lua.displayName = QStringLiteral("Lua");
            lua.keywords = split("and break do else elseif end for function goto if in local not or "
                                 "repeat return then until while");
            lua.builtins = split("true false nil print pairs ipairs type tostring tonumber require "
                                 "table string math io os");
            lua.lineComment = QStringLiteral("--");
            add(lua);

            Rules perl;
            perl.id = QStringLiteral("perl");
            perl.displayName = QStringLiteral("Perl");
            perl.keywords = split("my our local sub if elsif else unless while until for foreach do "
                                  "last next redo return package use require no bless wantarray");
            perl.lineComment = QStringLiteral("#");
            add(perl);

            Rules swift;
            swift.id = QStringLiteral("swift");
            swift.displayName = QStringLiteral("Swift");
            swift.keywords = split("associatedtype class deinit enum extension fileprivate func import "
                                   "init inout internal let open operator private protocol public "
                                   "rethrows static struct subscript typealias var break case continue "
                                   "default defer do else fallthrough for guard if in repeat return "
                                   "switch where while as catch is throw throws try async await");
            swift.builtins = split("Bool Int Double Float String Array Dictionary Set Optional true "
                                   "false nil self Self Any AnyObject");
            swift.lineComment = QStringLiteral("//");
            swift.blockCommentStart = QStringLiteral("/*");
            swift.blockCommentEnd = QStringLiteral("*/");
            add(swift);

            Rules r;
            r.id = QStringLiteral("r");
            r.displayName = QStringLiteral("R");
            r.keywords = split("if else repeat while function for in next break return");
            r.builtins = split("TRUE FALSE NULL NA Inf NaN c list data.frame matrix vector length "
                               "library require print");
            r.lineComment = QStringLiteral("#");
            add(r);

            Rules css;
            css.id = QStringLiteral("css");
            css.displayName = QStringLiteral("CSS");
            css.keywords = split("import media supports keyframes font-face charset include mixin "
                                 "extend use forward if else each for while return");
            css.builtins = split("inherit initial unset none auto flex grid block inline absolute "
                                 "relative fixed sticky hidden visible bold italic center left right");
            css.blockCommentStart = QStringLiteral("/*");
            css.blockCommentEnd = QStringLiteral("*/");
            css.lineComment = QStringLiteral("//"); // SCSS and LESS allow it
            add(css);

            Rules json;
            json.id = QStringLiteral("json");
            json.displayName = QStringLiteral("JSON");
            json.builtins = split("true false null");
            json.singleQuoted = false;
            json.keyValue = true;
            add(json);

            Rules xml;
            xml.id = QStringLiteral("xml");
            xml.displayName = QStringLiteral("XML");
            xml.markup = true;
            add(xml);

            return out;
        }();
        return table;
    }

    /// Suffix to language id. Kept apart from the table so several suffixes can
    /// share one definition without duplicating keyword lists.
    const QHash<QString, QString>& suffixTable()
    {
        static const QHash<QString, QString> table = [] {
            QHash<QString, QString> out;
            const auto map = [&out](const char* suffixes, const char* language) {
                const QStringList list = split(suffixes);
                for (const QString& suffix : list)
                    out.insert(suffix, QString::fromLatin1(language));
            };

            map("c h cc cpp cxx c++ hpp hxx hh inl ipp cu cuh m mm", "cpp");
            map("py pyw pyi", "python");
            map("js jsx mjs cjs ts tsx", "javascript");
            map("qml", "qml");
            map("java", "java");
            map("kt kts", "kotlin");
            map("cs", "csharp");
            map("go", "go");
            map("rs", "rust");
            map("rb rake gemspec", "ruby");
            map("php phtml", "php");
            map("sh bash zsh fish ksh", "shell");
            map("sql ddl", "sql");
            map("yaml yml", "yaml");
            map("ini toml cfg conf desktop service properties editorconfig gitconfig", "ini");
            map("cmake", "cmake");
            map("mk mak", "make");
            map("lua", "lua");
            map("pl pm t", "perl");
            map("swift", "swift");
            map("r rmd", "r");
            map("css scss sass less", "css");
            map("json jsonl geojson ipynb jsonc webmanifest", "json");
            map("xml html xhtml svg xsd xsl plist qrc ui vcxproj csproj pom gradle", "xml");
            return out;
        }();
        return table;
    }

    bool isWordCharacter(QChar c)
    {
        return c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('$');
    }

} // namespace

SourceHighlighter::SourceHighlighter(QObject* parent)
    : QSyntaxHighlighter(parent)
{
    m_key.setForeground(QColor(kKeyColour));
    m_string.setForeground(QColor(kStringColour));
    m_number.setForeground(QColor(kNumberColour));
    m_keyword.setForeground(QColor(kKeywordColour));
    m_builtin.setForeground(QColor(kBuiltinColour));
    m_tag.setForeground(QColor(kTagColour));
    m_attribute.setForeground(QColor(kAttributeColour));
    m_comment.setForeground(QColor(kCommentColour));
    m_comment.setFontItalic(true);
    m_preprocessor.setForeground(QColor(kPreprocessorColour));
}

QString SourceHighlighter::languageForSuffix(const QString& suffix)
{
    QString id = suffixTable().value(suffix.toLower());
    if (id.isEmpty())
        return {};
    return id;
}

QStringList SourceHighlighter::supportedLanguages()
{
    QStringList ids = languageTable().keys();
    ids.sort();
    return ids;
}

QStringList SourceHighlighter::knownSuffixes()
{
    QStringList suffixes = suffixTable().keys();
    suffixes.sort();
    return suffixes;
}

const SourceHighlighter::Rules* SourceHighlighter::rulesFor(const QString& languageId)
{
    const auto position = languageTable().find(languageId);
    return position == languageTable().end() ? nullptr : &position.value();
}

void SourceHighlighter::setLanguage(const QString& languageId)
{
    if (m_languageId == languageId)
        return;
    m_languageId = languageId;
    m_rules = rulesFor(languageId);
    rehighlight();
}

void SourceHighlighter::attachTo(QQuickTextDocument* document)
{
    QTextDocument* target = document ? document->textDocument() : nullptr;
    // Guarded, because setDocument() rehighlights, which changes the document,
    // which is exactly what a caller reacting to "the text changed" is
    // responding to. Attaching twice was an infinite recursion.
    if (target == QSyntaxHighlighter::document())
        return;
    setDocument(target);
}

void SourceHighlighter::highlightBlock(const QString& text)
{
    if (!m_rules) {
        setCurrentBlockState(StateNormal);
        return;
    }
    if (m_rules->markup) {
        highlightMarkup(text);
        return;
    }
    highlightCode(text, *m_rules);
}

int SourceHighlighter::highlightNumber(const QString& text, int start)
{
    int i = start;
    while (i < text.size()
        && (text.at(i).isDigit() || text.at(i) == QLatin1Char('.') || text.at(i) == QLatin1Char('x')
            || text.at(i) == QLatin1Char('X') || text.at(i) == QLatin1Char('_')
            || (text.at(i).isLetter() && text.at(i).toLower() <= QLatin1Char('f')))) {
        ++i;
    }
    setFormat(start, i - start, m_number);
    return i;
}

int SourceHighlighter::highlightString(const QString& text, int start, QChar quote)
{
    int i = start + 1;
    while (i < text.size()) {
        if (text.at(i) == QLatin1Char('\\')) {
            i += 2; // an escaped quote does not close the string
            continue;
        }
        if (text.at(i) == quote) {
            ++i;
            break;
        }
        ++i;
    }
    const int end = static_cast<int>(std::min<qsizetype>(i, text.size()));
    setFormat(start, end - start, m_string);
    return end;
}

void SourceHighlighter::highlightCode(const QString& text, const Rules& rules)
{
    int i = 0;
    int state = previousBlockState() == StateInBlockComment ? StateInBlockComment : StateNormal;

    // Finish a comment opened on an earlier line before looking at anything
    // else: everything up to the closing marker is comment, whatever it says.
    if (state == StateInBlockComment && !rules.blockCommentEnd.isEmpty()) {
        const int close = text.indexOf(rules.blockCommentEnd);
        if (close < 0) {
            setFormat(0, text.size(), m_comment);
            setCurrentBlockState(StateInBlockComment);
            return;
        }
        const int end = close + rules.blockCommentEnd.size();
        setFormat(0, end, m_comment);
        i = end;
        state = StateNormal;
    }

    // A preprocessor line is coloured whole; picking keywords out of `#ifdef`
    // would be noise.
    if (rules.id == QLatin1String("cpp")) {
        const QString trimmed = text.trimmed();
        if (trimmed.startsWith(QLatin1Char('#')) && i == 0) {
            setFormat(0, text.size(), m_preprocessor);
            setCurrentBlockState(StateNormal);
            return;
        }
    }

    while (i < text.size()) {
        const QChar c = text.at(i);

        if (!rules.lineComment.isEmpty() && text.mid(i, rules.lineComment.size()) == rules.lineComment) {
            setFormat(i, text.size() - i, m_comment);
            break;
        }
        if (!rules.altLineComment.isEmpty()
            && text.mid(i, rules.altLineComment.size()) == rules.altLineComment) {
            setFormat(i, text.size() - i, m_comment);
            break;
        }
        if (!rules.blockCommentStart.isEmpty()
            && text.mid(i, rules.blockCommentStart.size()) == rules.blockCommentStart) {
            const int close = text.indexOf(rules.blockCommentEnd, i + rules.blockCommentStart.size());
            if (close < 0) {
                setFormat(i, text.size() - i, m_comment);
                state = StateInBlockComment;
                break;
            }
            const int end = close + rules.blockCommentEnd.size();
            setFormat(i, end - i, m_comment);
            i = end;
            continue;
        }

        if ((c == QLatin1Char('"') && rules.doubleQuoted) || (c == QLatin1Char('\'') && rules.singleQuoted)
            || (c == QLatin1Char('`') && rules.backQuoted)) {
            i = highlightString(text, i, c);
            continue;
        }

        if (c.isDigit() && (i == 0 || !isWordCharacter(text.at(i - 1)))) {
            i = highlightNumber(text, i);
            continue;
        }

        if (isWordCharacter(c) && !c.isDigit()) {
            const int start = i;
            while (i < text.size() && isWordCharacter(text.at(i)))
                ++i;
            const QString word = text.mid(start, i - start);

            // A key in `key: value` is coloured as a key even though it is not
            // a keyword -- that is the structure the reader is looking for.
            if (rules.keyValue && !rules.builtins.contains(word)) {
                int after = i;
                while (after < text.size() && text.at(after).isSpace())
                    ++after;
                if (after < text.size() && text.at(after) == QLatin1Char(':')) {
                    setFormat(start, i - start, m_key);
                    continue;
                }
            }

            if (rules.keywords.contains(word))
                setFormat(start, i - start, m_keyword);
            else if (rules.builtins.contains(word))
                setFormat(start, i - start, m_builtin);
            continue;
        }

        ++i;
    }

    setCurrentBlockState(state);
}

void SourceHighlighter::highlightMarkup(const QString& text)
{
    static const QRegularExpression tag(QStringLiteral("</?\\s*([A-Za-z_][\\w:.-]*)"));
    static const QRegularExpression attribute(QStringLiteral("([\\w:.-]+)\\s*="));
    static const QRegularExpression value(QStringLiteral("\"[^\"]*\"|'[^']*'"));

    int state = previousBlockState() == StateInBlockComment ? StateInBlockComment : StateNormal;

    if (state == StateInBlockComment) {
        const int close = text.indexOf(QStringLiteral("-->"));
        if (close < 0) {
            setFormat(0, text.size(), m_comment);
            setCurrentBlockState(StateInBlockComment);
            return;
        }
        setFormat(0, close + 3, m_comment);
    }

    for (auto it = tag.globalMatch(text); it.hasNext();) {
        const auto match = it.next();
        setFormat(match.capturedStart(1), match.capturedLength(1), m_tag);
    }
    for (auto it = attribute.globalMatch(text); it.hasNext();) {
        const auto match = it.next();
        setFormat(match.capturedStart(1), match.capturedLength(1), m_attribute);
    }
    for (auto it = value.globalMatch(text); it.hasNext();) {
        const auto match = it.next();
        setFormat(match.capturedStart(), match.capturedLength(), m_string);
    }

    // Comments last: they win over anything the tag lexer coloured inside them.
    int from = 0;
    while (true) {
        const int open = text.indexOf(QStringLiteral("<!--"), from);
        if (open < 0)
            break;
        const int close = text.indexOf(QStringLiteral("-->"), open + 4);
        if (close < 0) {
            setFormat(open, text.size() - open, m_comment);
            state = StateInBlockComment;
            break;
        }
        setFormat(open, close + 3 - open, m_comment);
        from = close + 3;
        state = StateNormal;
    }

    setCurrentBlockState(state);
}

} // namespace mole
