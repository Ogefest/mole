#!/usr/bin/env bash
#
# What a release build's configure summary has to say, in one place.
#
# **Three consumers were carrying their own copy of this list** -- release.yml,
# scripts/package-appimage.sh, and tst_ReleaseWorkflow.sh, which holds one of them
# against the CMake messages -- and the two lists that matter had already drifted:
# the AppImage's was missing xxhash and the Multimedia QML module. A list that
# only one of three consumers is checked against is a list that goes stale in the
# other two. See MOLE-387 and TODO.md rule one.
#
# Sourced rather than run, and it sets two arrays:
#
#   MOLE_WANTED   -- summary lines a release build must print
#   MOLE_REFUSED  -- lines it must not print
#
# One argument, naming the artefact, because an artefact is allowed to be
# deliberately without something -- and every exemption is named here rather than
# discovered as a difference between two lists:
#
#   full      a local release build, the .deb and the .rpm: everything
#   appimage  the AppImage: no Parquet grid, and no Windows shares on purpose
#
# The Parquet absence is a property of the oldest distribution Mole runs on: EPEL
# 9 ships Arrow 9.0.0 and no ParquetConfig.cmake at all, so find_package(Parquet)
# cannot succeed there whatever is installed. The SMB absence is a licence
# decision -- libsmbclient is GPL-3.0-or-later and an AppImage is one artefact
# somebody is handed. See ADR-0094.

# -u like every other script here: reading an unset variable as empty is how a
# check silently looks for nothing. Sourced rather than run, so this sets it for
# the caller too -- which is what the callers want and both of them already do.
set -uo pipefail

MOLE_WANTED=()
MOLE_REFUSED=()

mole_feature_summary() {
    local artefact="${1:-full}"

    # Everything every artefact has. The strings are what src/app/CMakeLists.txt
    # and the plugin CMakeLists print, and tst_ReleaseWorkflow.sh holds this file
    # against them.
    MOLE_WANTED=(
        "Terminal: libvterm"
        "Git state: libgit2"
        "Duplicate scan head: xxhash"
        "Credential store: OpenSSL"
        "Network drives: sftp, ftp, s3, webdav"
        "NFS exports: nfs"
    )
    MOLE_REFUSED=(
        "Qt6 Pdf not found"
        "Qt6 Multimedia not found"
        "its QML module is not"
        "libarchive not found"
    )

    case "$artefact" in
        full)
            MOLE_WANTED+=("Parquet preview: enabled" "Windows shares: smb")
            ;;
        appimage)
            # Asserted as an absence rather than left out: libsmbclient-devel is
            # installed in that container, so a build that stopped passing
            # -DMOLE_WITH_SMB=OFF would silently find it and go out carrying
            # GPL-3 code.
            MOLE_WANTED+=("Windows shares: not built")
            ;;
        *)
            echo "unknown artefact: $artefact" >&2
            return 2
            ;;
    esac
}

# Checks a configure log against the two lists. Prints what is wrong and returns
# non-zero, so a caller can add its own words around it.
mole_check_summary() {
    local artefact="$1" log="$2" wrong=0 line
    mole_feature_summary "$artefact" || return 2

    for line in "${MOLE_WANTED[@]}"; do
        grep -qF "$line" "$log" || { echo "missing: $line"; wrong=1; }
    done
    for line in "${MOLE_REFUSED[@]}"; do
        grep -qF "$line" "$log" && { echo "not built with: $line"; wrong=1; }
    done
    return "$wrong"
}
