#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Turns a published Orbis-Win package into the release folder build-ps4-runtime.sh takes
# (ORBIS_WIN_RELEASE): <out>/BUILD-MANIFEST.txt and <out>/app/pkg-stage. The package comes from
# an Orbis-Win GitHub release (ORBIS_WIN_TAG) or from a folder holding one .pkg and its
# BUILD-MANIFEST*.txt (ORBIS_WIN_PKG_DIR, e.g. the runner cache before a release has it). Its
# SHA-256 must be the manifest's package_sha256; PkgTool then extracts it, and the extracted
# stage is the package's own files (sce_sys/param.sfo and icon0.png come out beside it).
#
#   scripts/ci/prepare-orbis-win.sh <sdk dir> <out dir>
set -euo pipefail

die() { echo "prepare-orbis-win: $*" >&2; exit 1; }
[[ $# -eq 2 ]] || die "usage: $0 <sdk dir> <out dir>"
SDK=$1 OUT=$2
PKGTOOL="$SDK/bin/linux/PkgTool.Core"
[[ -x "$PKGTOOL" ]] || die "PkgTool missing: $PKGTOOL"
[[ ! -e "$OUT" ]] || die "$OUT exists; give an empty path"
mkdir -p "$OUT/download"

if [[ -n "${ORBIS_WIN_TAG:-}" ]]; then
    repo=${ORBIS_WIN_REPO:-plastikman/orbis-win}
    gh release download "$ORBIS_WIN_TAG" -R "$repo" -D "$OUT/download" -p '*.pkg' -p 'BUILD-MANIFEST*.txt'
    source_note="release $repo@$ORBIS_WIN_TAG"
elif [[ -n "${ORBIS_WIN_PKG_DIR:-}" ]]; then
    cp "$ORBIS_WIN_PKG_DIR"/*.pkg "$ORBIS_WIN_PKG_DIR"/BUILD-MANIFEST*.txt "$OUT/download/"
    source_note="folder $ORBIS_WIN_PKG_DIR"
else
    die "set ORBIS_WIN_TAG (an Orbis-Win release) or ORBIS_WIN_PKG_DIR"
fi

shopt -s nullglob
pkgs=("$OUT"/download/*.pkg) manifests=("$OUT"/download/BUILD-MANIFEST*.txt)
[[ ${#pkgs[@]} -eq 1 ]] || die "expected one .pkg from the $source_note, found ${#pkgs[@]}"
[[ ${#manifests[@]} -eq 1 ]] || die "expected one BUILD-MANIFEST from the $source_note, found ${#manifests[@]}"
pkg=${pkgs[0]} manifest=${manifests[0]}

expected=$(sed -n 's/^package_sha256=//p' "$manifest")
[[ -n "$expected" ]] || die "manifest has no package_sha256"
actual=$(sha256sum "$pkg" | awk '{print $1}')
[[ "$actual" == "$expected" ]] || die "package SHA-256 $actual is not the manifest's $expected"

DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 "$PKGTOOL" pkg_extract --passcode 00000000000000000000000000000000 \
    "$pkg" "$OUT/extract" > "$OUT/extract.log" 2>&1 || { tail -20 "$OUT/extract.log" >&2; die "PkgTool could not extract $pkg"; }
[[ -f "$OUT/extract/uroot/eboot.bin" ]] || die "the extracted package has no uroot/eboot.bin"

# The manifest's per-file hashes (sha256 <hash> <path>) must hold for the extracted files.
checked=0
while read -r _ hash path; do
    [[ -f "$OUT/extract/uroot/$path" ]] || die "manifest file missing from the package: $path"
    [[ "$(sha256sum "$OUT/extract/uroot/$path" | awk '{print $1}')" == "$hash" ]] ||
        die "manifest hash mismatch for $path"
    checked=$((checked + 1))
done < <(awk '$1 == "sha256" && NF == 3' "$manifest")
[[ $checked -gt 0 ]] || die "the manifest lists no file hashes"

# The package's keystone is the packager's, made again for Playden's package; build-ps4-runtime.sh
# copies the whole stage, so it must not come along.
rm -f "$OUT/extract/uroot/sce_sys/keystone"
mkdir -p "$OUT/app"
mv "$OUT/extract/uroot" "$OUT/app/pkg-stage"
cp "$manifest" "$OUT/BUILD-MANIFEST.txt"
echo "orbis_win_source=$source_note" >> "$OUT/BUILD-MANIFEST.txt"
echo "prepare-orbis-win: $(basename "$pkg") from the $source_note, $checked manifest hashes checked"
