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
    cp "$ORBIS_WIN_PKG_DIR"/*.pkg "$OUT/download/"
    if compgen -G "$ORBIS_WIN_PKG_DIR/BUILD-MANIFEST*.txt" >/dev/null; then
        cp "$ORBIS_WIN_PKG_DIR"/BUILD-MANIFEST*.txt "$OUT/download/"
    fi
    source_note="${ORBIS_WIN_PACKAGE_TAG:+release $ORBIS_WIN_PACKAGE_TAG}"
    source_note="${source_note:-folder $ORBIS_WIN_PKG_DIR}"
else
    die "set ORBIS_WIN_TAG (an Orbis-Win release) or ORBIS_WIN_PKG_DIR"
fi

shopt -s nullglob
pkgs=("$OUT"/download/*.pkg) manifests=("$OUT"/download/BUILD-MANIFEST*.txt)
[[ ${#pkgs[@]} -eq 1 ]] || die "expected one .pkg from the $source_note, found ${#pkgs[@]}"
pkg=${pkgs[0]}
if [[ ${#manifests[@]} -eq 0 && -n "${ORBIS_WIN_PACKAGE_LOCK:-}" ]]; then
    lock=$ORBIS_WIN_PACKAGE_LOCK
    [[ -f "$lock" ]] || die "package lock missing: $lock"
    lock_release=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["release"])' "$lock")
    lock_pkg=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["package"])' "$lock")
    lock_hash=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["package_sha256"])' "$lock")
    lock_mode=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["provenance_mode"])' "$lock")
    [[ "$lock_mode" == release-package-only ]] || die "unsupported package lock provenance mode: $lock_mode"
    [[ -z "${ORBIS_WIN_TAG:-}" || "$ORBIS_WIN_TAG" == "$lock_release" ]] ||
        die "package lock is for $lock_release, not ${ORBIS_WIN_TAG}"
    [[ "$(basename "$pkg")" == "$lock_pkg" ]] || die "package filename does not match lock: $(basename "$pkg")"
    actual=$(sha256sum "$pkg" | awk '{print $1}')
    [[ "$actual" == "$lock_hash" ]] || die "package SHA-256 $actual does not match locked $lock_hash"
    : > "$OUT/BUILD-MANIFEST.txt"
    while IFS=$'\t' read -r key value; do printf '%s=%s\n' "$key" "$value" >> "$OUT/BUILD-MANIFEST.txt"; done < <(
        python3 - "$lock" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
for src, dst in (("release", "release_tag"), ("root_commit", "root_commit"),
                 ("package_sha256", "package_sha256"), ("title_id", "title_id"),
                 ("app_version", "app_version"), ("runtime_build_marker", "runtime_build_marker"),
                 ("provenance_mode", "provenance_mode")):
    print(f"{dst}\t{d[src]}")
PY
    )
    manifest="$OUT/BUILD-MANIFEST.txt"
elif [[ ${#manifests[@]} -eq 1 ]]; then
    manifest=${manifests[0]}
else
    die "expected one BUILD-MANIFEST from the $source_note, found ${#manifests[@]}"
fi

expected=$(sed -n 's/^package_sha256=//p' "$manifest")
[[ -n "$expected" ]] || die "manifest has no package_sha256"
actual=$(sha256sum "$pkg" | awk '{print $1}')
[[ "$actual" == "$expected" ]] || die "package SHA-256 $actual is not the manifest's $expected"

DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 "$PKGTOOL" pkg_extract --passcode 00000000000000000000000000000000 \
    "$pkg" "$OUT/extract" > "$OUT/extract.log" 2>&1 || { tail -20 "$OUT/extract.log" >&2; die "PkgTool could not extract $pkg"; }
[[ -f "$OUT/extract/uroot/eboot.bin" ]] || die "the extracted package has no uroot/eboot.bin"

if [[ "$(sed -n 's/^provenance_mode=//p' "$manifest")" == release-package-only ]]; then
    marker=$(sed -n 's/^runtime_build_marker=//p' "$manifest")
    [[ -n "$marker" ]] || die "locked package manifest has no runtime_build_marker"
    grep -aFq "$marker" "$OUT/extract/uroot/sce_module/relocated_wine_app.prx" ||
        die "runtime marker $marker is absent from the locked package"
    for path in sce_module/libvulkan.prx sce_module/relocated_wine_app.prx; do
        [[ -s "$OUT/extract/uroot/$path" ]] || die "locked package lacks $path"
        if ! awk -v path="$path" '$1 == "sha256" && $3 == path { found = 1 } END { exit !found }' "$manifest"; then
            printf 'sha256 %s %s\n' "$(sha256sum "$OUT/extract/uroot/$path" | awk '{print $1}')" "$path" >> "$manifest"
        fi
    done
fi

# Older Orbis-Win builds list every payload file as `sha256 <hash> <path>`.
# The v0.0.6 tested-stage release instead binds its package with package_sha256
# and records the source stage/tree digests. The package digest was verified
# above before extraction, so accept that manifest format while retaining the
# per-file checks whenever the records are present.
checked=0
while read -r _ hash path; do
    [[ -f "$OUT/extract/uroot/$path" ]] || die "manifest file missing from the package: $path"
    [[ "$(sha256sum "$OUT/extract/uroot/$path" | awk '{print $1}')" == "$hash" ]] ||
        die "manifest hash mismatch for $path"
    checked=$((checked + 1))
done < <(awk '$1 == "sha256" && NF == 3' "$manifest")
if [[ $checked -eq 0 ]]; then
    stage_hash=$(sed -n 's/^stage_tree_sha256=//p' "$manifest")
    source_hash=$(sed -n 's/^source_stage_sha256=//p' "$manifest")
    if [[ "$stage_hash" =~ ^[0-9a-f]{64}$ && "$source_hash" =~ ^[0-9a-f]{64}$ ]]; then
        echo "prepare-orbis-win: package SHA-256 verified; manifest records tested-stage digests"
    else
        release_version=$(sed -n 's/^release_version=//p' "$manifest")
        base_hash=$(sed -n 's/^base_package_sha256=//p' "$manifest")
        mmdevapi32=$(sed -n 's/^mmdevapi32_sha256=//p' "$manifest")
        mmdevapi64=$(sed -n 's/^mmdevapi64_sha256=//p' "$manifest")
        [[ "$release_version" == 0.0.7-candidate && \
           "$base_hash" =~ ^[0-9a-f]{64}$ && \
           "$mmdevapi32" =~ ^[0-9a-f]{64}$ && \
           "$mmdevapi64" =~ ^[0-9a-f]{64}$ ]] ||
            die "manifest has neither per-file hashes, tested-stage digests, nor recognized v0.0.7 MMDevAPI provenance"
        echo "prepare-orbis-win: package SHA-256 verified; manifest records v0.0.7 base-package and MMDevAPI hashes"
    fi
fi

# The package's keystone is the packager's, made again for Playden's package; build-ps4-runtime.sh
# copies the whole stage, so it must not come along.
rm -f "$OUT/extract/uroot/sce_sys/keystone"
mkdir -p "$OUT/app"
mv "$OUT/extract/uroot" "$OUT/app/pkg-stage"
[[ "$manifest" == "$OUT/BUILD-MANIFEST.txt" ]] || cp "$manifest" "$OUT/BUILD-MANIFEST.txt"
echo "orbis_win_source=$source_note" >> "$OUT/BUILD-MANIFEST.txt"
echo "prepare-orbis-win: $(basename "$pkg") from the $source_note, $checked manifest hashes checked"
