#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
sdk="$root/deps/orbis-sdk-v1/sdk"
compat="$root/deps/orbis-sdk-v1/orbis-compat"
mesa="$root/deps/orbis-sdk-v1/mesa"
example="$root/deps/orbis-sdk-v1/examples/triangle"
toolchain="$compat/cmake/ps4-openorbis.cmake"
build="$root/build/ps4-vulkan-probe"
[[ -f "$sdk/link.x" && -f "$mesa/build-orbis/src/amd/vulkan/libvulkan_radeon.a" ]] || {
  echo "paired PS4 SDK/Mesa bundle missing" >&2; exit 1;
}
export OO_PS4_TOOLCHAIN="$sdk" ORBIS_COMPAT_DIR="$compat"
cmake -S "$example" -B "$build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DOO_PS4_TOOLCHAIN="$sdk" \
  -DORBIS_COMPAT_DIR="$compat" -DORBIS_MESA_SRC="$mesa" \
  -DORBIS_MESA_BUILD="$mesa/build-orbis" -DORBIS_KIT_DIR="$compat"
cmake --build "$build" --target triangle
"$sdk/bin/linux/create-fself" -in "$build/triangle" \
  -out "$build/triangle.oelf" -eboot "$build/eboot.bin"

modules="$build/modules"
mkdir -p "$modules"
for name in libc libSceFios2; do
  mkdir -p "$modules/$name"
  cp -a "$root/ps4/module-stubs/$name/." "$modules/$name/"
  make -C "$modules/$name" clean all OO_PS4_TOOLCHAIN="$sdk"
done

"$compat/scripts/ps4/make-pkg.sh" \
  --eboot "$build/eboot.bin" --out-dir "$build/package" \
  --title-id PLDN10001 --title "Playden for PS4" --sdk "$sdk" \
  --extra "$modules/libc/libc.prx:sce_module/libc.prx" \
  --extra "$modules/libSceFios2/libSceFios2.prx:sce_module/libSceFios2.prx"
