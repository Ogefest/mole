#!/usr/bin/env python3
"""Backticked identifiers in a document that no longer exist in the code.

`docs/WRITING_PLUGINS.md` is the one document a third party works from, and every
name in it is a promise. Several were stale in ways nobody could see by reading
either half alone: a sample carrying a class from a layer above the SDK, a method
the interface had gained and the guide had not, a menu section named after an enum
value that had been renamed.

What counts as an identifier is deliberately narrow, because a document is mostly
prose in backticks -- `make bundle`, `latest.json`, `Ctrl+F`. A token is asked
about only when it looks like C++ and nothing else: it carries `::`, or it ends
with `()`, or it is UPPER_SNAKE, or it is CamelCase with no spaces. Anything else
is left alone, and a name that is spelled like C++ but is not in the code is
exactly what this is for.

**The directories given are the layers the document's reader can see**, and that
is the second half of what this catches. `docs/WRITING_PLUGINS.md` is read
against `src/sdk` and `src/core` only, because those are the layers a plugin
links -- so a sample carrying `mole::FileListModel`, which lives in `src/ui`,
is reported as a name its reader cannot have. That is not a spelling mistake; it
is the guide teaching the layering being broken, and it was in the tab sample.

Two kinds of name are looked up in `tests/` as well, because a document may
legitimately name the suite: a token qualified with a test class
(`tst_AppIntegration::everyFeatureIsReachableFromTheMenu`) and anything in the
`mole::test::` namespace (the conformance harness). Neither is a layer a plugin
links, and both are things a plugin author is told to use.

Usage:
  read-doc-identifiers.py <document> [<visible layer> ...]

Prints one unknown identifier per line and exits 1 when there are any; exits 0
when the document names nothing its reader cannot have.

See MOLE-392.
"""

import re
import sys
from pathlib import Path

# Fenced blocks are read too: the samples are where a stale name does the most
# damage, because somebody copies them.
BACKTICKED = re.compile(r"`([^`\n]+)`")
FENCE = re.compile(r"^```")

IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*(::[A-Za-z_][A-Za-z0-9_]*)*(\(\))?$")
CAMEL = re.compile(r"^[a-z]+([A-Z][a-z0-9]*)+$|^[A-Z][a-z0-9]*([A-Z][a-z0-9]*)+$")
UPPER = re.compile(r"^[A-Z][A-Z0-9_]{2,}$")

# Spelled like C++ and not this project's: Qt's own types, the language's, and the
# few words that happen to be CamelCase in prose.
NOT_OURS = {
    "QObject", "QString", "QStringList", "QVariant", "QVariantMap", "QWidget",
    "QImage", "QIcon", "QUrl", "QDateTime", "QByteArray", "QByteArrayView",
    "QJsonObject", "QQuickItem", "QAbstractListModel", "QSqlDatabase",
    "QTemporaryDir", "QProcess", "QTimer", "QPointer", "QCoreApplication",
    "QGuiApplication", "QApplication", "QDir", "QFile", "QFileInfo", "QList",
    "QHash", "QMap", "QSet", "QPair", "QThread", "QMutex", "QElapsedTimer",
    "QPluginLoader", "QLibrary", "QMetaObject", "QtConcurrent", "QRegularExpression",
    "Q_OBJECT", "Q_PROPERTY", "Q_INVOKABLE", "Q_ENUM", "Q_INTERFACES",
    "Q_PLUGIN_METADATA", "Q_DECLARE_INTERFACE", "Q_EMIT", "Q_SIGNALS", "Q_SLOTS",
    # Qt's own CMake keywords, which a guide's build sample is full of.
    "CLASS_NAME", "LIBRARY_OUTPUT_DIRECTORY", "PRIVATE", "PUBLIC", "SHARED", "FILE",
    "CMakeLists", "GitHub", "JavaScript", "TypeScript", "CamelCase", "README",
    "TODO", "ADR", "SDK", "ABI", "API", "QML", "UTF", "JSON", "HTTP", "HTTPS",
    "SFTP", "WebDAV", "NFS", "SMB", "PDF", "EXIF", "MOLE_PLUGIN_PATH",
}


def looks_like_code(token):
    if not IDENTIFIER.match(token):
        return False
    if "::" in token or token.endswith("()"):
        return True
    return bool(CAMEL.match(token) or UPPER.match(token))


def identifiers(text):
    """Every backticked token in the document that looks like C++."""
    found = []
    for raw in BACKTICKED.findall(text):
        token = raw.strip()
        if looks_like_code(token):
            found.append(token)
    # Fenced samples are not backticked line by line; take their words too.
    inside = False
    for line in text.splitlines():
        if FENCE.match(line):
            inside = not inside
            continue
        if not inside:
            continue
        for token in re.findall(r"[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*(?:\(\))?", line):
            if "::" in token and looks_like_code(token):
                found.append(token)
    return found


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    document = Path(sys.argv[1])
    root = Path(__file__).resolve().parents[2]
    directories = [root / piece for piece in (sys.argv[2:] or ["src"])]

    # Comments are stripped, and that is the point rather than tidiness: a name
    # that survives only in a comment is exactly the kind of stale identifier
    # this looks for -- `Tools` was a menu section that had been renamed
    # Operations and Workflows, and the enum's own comment still said Tools, so a
    # search that read comments would have called the guide right.
    def read(directories):
        text = []
        for directory in directories:
            for path in sorted(Path(directory).rglob("*")):
                if path.suffix in (".h", ".cpp", ".qml", ".txt"):
                    body = path.read_text(encoding="utf-8", errors="replace")
                    body = re.sub(r"/\*.*?\*/", " ", body, flags=re.S)
                    body = re.sub(r"//[^\n]*", " ", body)
                    if path.suffix == ".txt":
                        body = re.sub(r"(?m)^\s*#[^\n]*", " ", body)
                    text.append(body)
        return "\n".join(text)

    code = []
    for directory in directories:
        for path in sorted(directory.rglob("*")):
            if path.suffix in (".h", ".cpp", ".qml", ".txt"):
                text = path.read_text(encoding="utf-8", errors="replace")
                text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
                text = re.sub(r"//[^\n]*", " ", text)
                if path.suffix == ".txt":
                    # CMake comments. Not for C++, where a `#` line is a
                    # preprocessor directive and MOLE_PLUGIN_IID is one.
                    text = re.sub(r"(?m)^\s*#[^\n]*", " ", text)
                code.append(text)
    haystack = "\n".join(code)
    if not haystack:
        print("no source was read at all, so this checked nothing", file=sys.stderr)
        return 2

    # The suite, for the two kinds of name a document may take from it.
    suite = read([root / "tests"]) if (root / "tests").is_dir() else ""

    unknown = []
    for token in identifiers(document.read_text(encoding="utf-8")):
        # The last component is what the code declares: `mole::IFeature::openAt()`
        # is declared as `openAt`.
        name = token.removesuffix("()").split("::")[-1]
        if name in NOT_OURS or token in NOT_OURS:
            continue
        where = haystack
        parts = token.split("::")
        if len(parts) > 1:
            # A qualified name says whose it is. `mole::…` is a promise about this
            # project and is checked; `tst_…::…` and `mole::test::…` are about the
            # suite and are checked there; anything else is the reader's own code
            # in a sample -- `TestGitLabFs::conformance()` is not a name this
            # repository is supposed to have.
            if parts[0].startswith("tst_") or "test" in parts[:-1]:
                where = haystack + "\n" + suite
            elif parts[0] != "mole":
                continue
        if not re.search(r"\b%s\b" % re.escape(name), where):
            unknown.append(token)

    for token in sorted(set(unknown)):
        print(token)
    return 1 if unknown else 0


if __name__ == "__main__":
    sys.exit(main())
