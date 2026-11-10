#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
sdk="$root/deps/orbis-sdk-v1/sdk"
toolchain="$root/deps/orbis-sdk-v1/orbis-compat/cmake/ps4-openorbis.cmake"
compat="$root/deps/orbis-sdk-v1/orbis-compat"
build="$root/build/ps4-shell"
package=0
if [[ "${1:-}" == "--pkg" ]]; then package=1; shift; fi
[[ $# -eq 0 ]] || { echo "usage: $0 [--pkg]" >&2; exit 2; }
[[ -f "$sdk/link.x" ]] || { echo "PS4 SDK missing at $sdk" >&2; exit 1; }
export OO_PS4_TOOLCHAIN="$sdk" ORBIS_COMPAT_DIR="$compat"
cmake -S "$root/ps4" -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DOO_PS4_TOOLCHAIN="$sdk" \
    -DORBIS_COMPAT_DIR="$compat" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$build"
"$sdk/bin/linux/create-fself" -in "$build/playden-ps4" \
    -out "$build/playden-ps4.oelf" -eboot "$build/eboot.bin"
echo "PS4 eboot: $build/eboot.bin"
if (( package )); then
    modules="$build/modules"
    mkdir -p "$modules"
    for name in libc libSceFios2; do
        mkdir -p "$modules/$name"
        cp -a "$root/ps4/module-stubs/$name/." "$modules/$name/"
        OO_PS4_TOOLCHAIN="$sdk" make -C "$modules/$name" clean all
        cp "$modules/$name/$name.prx" "$modules/$name.prx"
    done
    "$root/deps/orbis-sdk-v1/orbis-compat/scripts/ps4/make-pkg.sh" \
        --eboot "$build/eboot.bin" --out-dir "$build/package" \
        --title-id PLDN10001 --title "Playden for PS4" --sdk "$sdk" \
        --extra "$modules/libc.prx:sce_module/libc.prx" \
        --extra "$modules/libSceFios2.prx:sce_module/libSceFios2.prx" \
        --extra "$root/third_party/ps5-homebrew-ui/assets/fonts/inter-regular.huifont:assets/inter-regular.huifont" \
        --extra "$root/scripts/network-deps/.deps/ps4/cacert.pem:assets/cacert.pem"
fi
