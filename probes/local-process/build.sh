#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the native PS4 local-process probe. Set PROBE_MAC_IP to receive a UDP copy of its logs.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)"
sdk="$root/deps/orbis-sdk-v1/sdk"
compat="$root/deps/orbis-sdk-v1/orbis-compat"
toolchain="$compat/cmake/ps4-openorbis.cmake"
build="$root/build/local-process-ps4"
package=OFF
if [[ "${1:-}" == "--pkg" ]]; then package=ON; shift; fi
[[ $# -eq 0 ]] || { echo "usage: $0 [--pkg]" >&2; exit 2; }
[[ -f "$sdk/link.x" ]] || { echo "PS4 SDK missing at $sdk" >&2; exit 1; }
export OO_PS4_TOOLCHAIN="$sdk" ORBIS_COMPAT_DIR="$compat"
cmake -S "$root/probes/local-process/ps4" -B "$build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DOO_PS4_TOOLCHAIN="$sdk" \
  -DORBIS_COMPAT_DIR="$compat" -DPS4_BUILD_PKG="$package" \
  -DPROBE_MAC_IP="${PROBE_MAC_IP:-0.0.0.0}"
cmake --build "$build"
echo "PS4 probe eboot: $build/eboot.bin"
if [[ "$package" == ON ]]; then
  echo "PS4 probe package: $build/IV0000-TSTP00001_00-LOCALPROCESS.pkg"
fi
