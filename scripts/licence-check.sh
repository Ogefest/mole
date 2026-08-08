#!/usr/bin/env bash
#
# Verifies the conditions that let an Apache-2.0 application use LGPL Qt.
# Run it before publishing a build; see docs/LICENSING.md for the reasoning.
set -uo pipefail

BIN="${1:-build/release/mole}"
fail=0
ok()   { printf '  \033[32mOK\033[0m    %s\n' "$1"; }
bad()  { printf '  \033[31mFAIL\033[0m  %s\n' "$1"; fail=1; }

echo "Licence check: $BIN"

if [[ ! -x "$BIN" ]]; then
    bad "no binary at $BIN (run: make release)"
    exit 1
fi

# 1. Qt must be dynamically linked. Static linking would drag the application
#    itself under LGPL relinking obligations.
qt_shared=$(ldd "$BIN" 2>/dev/null | grep -c 'libQt6')
if (( qt_shared > 0 )); then
    ok "Qt is dynamically linked ($qt_shared libraries)"
else
    bad "no dynamically linked Qt found -- is it statically linked?"
fi

# 2. Qt symbols must live in the shared libraries, not inside our binary.
if nm -C --defined-only "$BIN" 2>/dev/null | grep -qE ' T (QQuickItem|QQmlEngine|QCoreApplication)::'; then
    bad "Qt symbols are defined inside the binary -- Qt appears to be static"
else
    ok "no Qt symbols compiled into the binary"
fi

# 3. Only LGPL Qt modules. Charts, DataVisualization, VirtualKeyboard and
#    friends are GPL-or-commercial and would change the answer entirely.
gpl_only=$(grep -rhoE 'Qt6::(Charts|DataVisualization|VirtualKeyboard|Lottie|Quick3DAssetImport)' \
           --include=CMakeLists.txt . | sort -u)
if [[ -z "$gpl_only" ]]; then
    ok "no GPL-only Qt modules are used"
else
    bad "GPL-only Qt module in use: $gpl_only"
fi

# 4. The paperwork has to travel with the build.
for f in LICENSE NOTICE THIRD-PARTY-NOTICES.md licenses/LGPL-3.0.txt licenses/Apache-2.0.txt; do
    [[ -f "$f" ]] && ok "present: $f" || bad "missing: $f"
done

# 5. In a bundle, Qt has to remain replaceable.
if [[ -d dist/usr/lib ]]; then
    if [[ -w dist/usr/lib/libQt6Core.so.6 ]] && grep -q LD_LIBRARY_PATH dist/mole 2>/dev/null; then
        ok "bundled Qt is writable and found via LD_LIBRARY_PATH (replaceable)"
    else
        bad "bundled Qt cannot be replaced by the user"
    fi
fi

echo
if (( fail )); then
    echo "Not compliant. See docs/LICENSING.md."
    exit 1
fi
echo "Compliant."
