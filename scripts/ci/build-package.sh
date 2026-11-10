#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the PLDN10001 package the way a workstation build does, from published inputs only:
#   - ORBIS_WIN_ROOT: an Orbis-Win checkout at its package's root_commit, prepared by its own
#     scripts/ci/prepare-runner.sh (toolchain, SDK, host Wine and PRX stubs from the runner cache);
#   - the Orbis-Win package itself (ORBIS_WIN_TAG, a release, or ORBIS_WIN_PKG_DIR), extracted by
#     scripts/ci/prepare-orbis-win.sh;
#   - DXVK 2.6.2 and the PS4 network libraries, downloaded and checked against pinned hashes.
# PKG_VERSION (NN.NN) is the package's APP_VER. OUT receives the package and its manifests.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
die() { echo "build-package: $*" >&2; exit 1; }
: "${ORBIS_WIN_ROOT:?set ORBIS_WIN_ROOT to the prepared Orbis-Win checkout}"
: "${PKG_VERSION:?set PKG_VERSION (NN.NN)}"
: "${OUT:?set OUT to an empty output folder}"
[[ "$PKG_VERSION" =~ ^[0-9]{2}\.[0-9]{2}$ ]] || die "PKG_VERSION '$PKG_VERSION' must be NN.NN"
WORK="${WORK:-$ROOT/build/ci}"
SDK_ROOT="$ORBIS_WIN_ROOT/sdk/orbis-sdk-v1"
[[ -f "$SDK_ROOT/sdk/link.x" ]] || die "Orbis-Win SDK missing under $SDK_ROOT"
rm -rf "$WORK" && mkdir -p "$WORK" "$OUT"

# The PS4 network libraries build against the SDK at deps/orbis-sdk-v1; on a runner that is
# Orbis-Win's (its only change, a libc++ header, is not used by the C libraries).
if [[ ! -e "$ROOT/deps/orbis-sdk-v1" ]]; then
    mkdir -p "$ROOT/deps"
    ln -s "$SDK_ROOT" "$ROOT/deps/orbis-sdk-v1"
fi

echo "== DXVK 2.6.2"
dxvk_sha256=17761876556afd55736cb895d184f5a1c55d43350f1b1e3b129f8d28706d7992
curl --fail --location --proto '=https' --tlsv1.2 -sS -o "$WORK/dxvk-2.6.2.tar.gz" \
    https://github.com/doitsujin/dxvk/releases/download/v2.6.2/dxvk-2.6.2.tar.gz
[[ "$(sha256sum "$WORK/dxvk-2.6.2.tar.gz" | awk '{print $1}')" == "$dxvk_sha256" ]] ||
    die "DXVK 2.6.2 archive hash mismatch"
tar -xzf "$WORK/dxvk-2.6.2.tar.gz" -C "$WORK"

echo "== PS4 network libraries"
"$ROOT/scripts/network-deps/prepare-ps4-deps.sh"

echo "== Orbis-Win package"
PS4_PKGTOOL_OPENSSL_LIB="${PS4_PKGTOOL_OPENSSL_LIB:-}"
LD_LIBRARY_PATH="${PS4_PKGTOOL_OPENSSL_LIB}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$ROOT/scripts/ci/prepare-orbis-win.sh" "$SDK_ROOT/sdk" "$WORK/orbis-win"
root_commit=$(sed -n 's/^root_commit=//p' "$WORK/orbis-win/BUILD-MANIFEST.txt")
checkout_commit=$(git -C "$ORBIS_WIN_ROOT" rev-parse HEAD)
[[ "$root_commit" == "$checkout_commit" ]] ||
    die "Orbis-Win checkout is $checkout_commit; its package was built from $root_commit"

echo "== PLDN10001 $PKG_VERSION"
ORBIS_WIN_RELEASE="$WORK/orbis-win" DXVK_262_DIR="$WORK/dxvk-2.6.2/x32" \
    "$ROOT/scripts/build-ps4-runtime.sh"

pkg="$ROOT/build/ps4-runtime/package/IV0000-PLDN10001_00-PLDN100010000000.pkg"
[[ -s "$pkg" ]] || die "no package at $pkg"
cp "$pkg" "$OUT/"
cp "$ROOT/build/ps4-runtime/pkg-stage/runtime-manifest.txt" "$OUT/runtime-manifest.txt"
{
    echo "title_id=PLDN10001"
    echo "version=$PKG_VERSION"
    echo "package=$(basename "$pkg")"
    echo "package_sha256=$(sha256sum "$pkg" | awk '{print $1}')"
    echo "playden_commit=$(git -C "$ROOT" rev-parse HEAD)"
    echo "playden_status=$(git -C "$ROOT" status --porcelain --untracked-files=no | sha256sum | cut -c1-64)"
    echo "orbis_win_root_commit=$root_commit"
    sed -n -e 's/^orbis_win_source=/&/p' -e 's/^package_sha256=/orbis_win_&/p' -e 's/^title_id=/orbis_win_&/p' \
        "$WORK/orbis-win/BUILD-MANIFEST.txt"
    echo "dxvk_sha256=$dxvk_sha256"
    git -C "$ROOT" submodule status third_party/prospero-win third_party/ps5-homebrew-ui | sed 's/^/submodule /'
} > "$OUT/BUILD-MANIFEST.txt"
cat "$OUT/BUILD-MANIFEST.txt"
