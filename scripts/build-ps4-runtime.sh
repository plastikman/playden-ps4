#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
ORBIS_WIN_ROOT="${ORBIS_WIN_ROOT:?set ORBIS_WIN_ROOT to the hardware-tested orbis-win checkout}"
ORBIS_WIN_RELEASE="${ORBIS_WIN_RELEASE:?set ORBIS_WIN_RELEASE to the selected Orbis-Win release stage}"
ORBIS_WIN_STAGE="${ORBIS_WIN_STAGE:-$ORBIS_WIN_RELEASE/app/pkg-stage}"
# DXVK_DIR is the x32 DLL directory; DXVK_262_DIR is the old name and still works.
# The version is read back out of the DLLs for the manifest, never assumed: a build
# with DXVK 3.1.1 in it used to still record dxvk=2.6.2, because nothing checked.
DXVK_DIR="${DXVK_DIR:-${DXVK_262_DIR:-}}"
[[ -n "$DXVK_DIR" ]] || { echo "build-ps4-runtime: set DXVK_DIR to the DXVK x32 DLLs" >&2; exit 1; }
DXVK_262_DIR="$DXVK_DIR"
# 64-bit games load D3D from system32, so the template needs DXVK's x64 DLLs there too.
DXVK_262_X64_DIR="${DXVK_262_X64_DIR:-$(dirname "$DXVK_262_DIR")/x64}"
PW="$ORBIS_WIN_ROOT/prospero-win"
SDK="$ORBIS_WIN_ROOT/sdk/orbis-sdk-v1/sdk"
COMPAT="$ORBIS_WIN_ROOT/sdk/orbis-sdk-v1/orbis-compat"
CC="$ORBIS_WIN_ROOT/build/ps4-wine-sdk/bin/prospero-clang"
BUILD="${PLAYDEN_PS4_BUILD_DIR:-$ROOT/build/ps4-runtime}"
CMAKE_BUILD="$BUILD/cmake"
STAGE="$BUILD/pkg-stage"
PKG_OUT="$BUILD/package"
TITLE_ID=PLDN10001
PKG_VERSION="${PKG_VERSION:-01.24}"
MESA_SHA256="${PLAYDEN_MESA_SHA256:-}"
WINE_MONO_DIR="${WINE_MONO_DIR:-}"
WINE_MONO_VERSION="${WINE_MONO_VERSION:-11.3.0}"
VKD3D_PROTON_DIR="${VKD3D_PROTON_DIR:-}"
VKD3D_PROTON_VERSION="${VKD3D_PROTON_VERSION:-none}"
VKD3D_PROTON_QUALIFIED="${VKD3D_PROTON_QUALIFIED:-0}"
KIT_ID="orbis-win-ps4"
export WINE_MONO_VERSION VKD3D_PROTON_QUALIFIED
PKGTOOL_SSL_LIB="${PS4_PKGTOOL_OPENSSL_LIB:-${HOME}/.cache/orbis-ports/openssl11/lib}"
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
if [[ -f "$PKGTOOL_SSL_LIB/libssl.so.1.1" ]]; then
    export LD_LIBRARY_PATH="$PKGTOOL_SSL_LIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export PS4_PKGTOOL_OPENSSL_LIB="$PKGTOOL_SSL_LIB"
fi

die() { echo "build-ps4-runtime: $*" >&2; exit 1; }

PW_SOURCE_COMMIT="${PW_SOURCE_COMMIT:-}"
RELEASE_MANIFEST="$ORBIS_WIN_RELEASE/BUILD-MANIFEST.txt"
[[ -f "$RELEASE_MANIFEST" ]] || die "missing release manifest: $RELEASE_MANIFEST"
HOST_WINE_ROOT="$(sed -n 's/^host_wine=//p' "$RELEASE_MANIFEST")"
[[ "$HOST_WINE_ROOT" == /* ]] || die "release manifest has no absolute host_wine path"
manifest_diff="$(sed -n 's/^prospero_win_abi4_diff_sha256=//p' "$RELEASE_MANIFEST")"
empty_diff_sha256=e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
[[ "$manifest_diff" == "$empty_diff_sha256" ]] ||
    die "Orbis-Win release contains local changes (prospero_win_abi4_diff_sha256=$manifest_diff)"
manifest_root_diff="$(sed -n 's/^orbis_win_root_diff_sha256=//p' "$RELEASE_MANIFEST")"
[[ "$manifest_root_diff" == "$empty_diff_sha256" ]] ||
    die "Orbis-Win root release contains local changes (orbis_win_root_diff_sha256=${manifest_root_diff:-missing})"
manifest_commit="$(sed -n 's/^prospero_win_abi4_commit=//p' "$RELEASE_MANIFEST")"
[[ -n "$manifest_commit" ]] || die "missing exact Orbis-Win source commit from BUILD-MANIFEST.txt"
manifest_root_commit="$(sed -n 's/^root_commit=//p' "$RELEASE_MANIFEST")"
[[ -n "$manifest_root_commit" ]] || die "missing exact Orbis-Win root commit from BUILD-MANIFEST.txt"
actual_root_commit="$(git -C "$ORBIS_WIN_ROOT" rev-parse HEAD)"
[[ "$actual_root_commit" == "$manifest_root_commit" ]] ||
    die "Orbis-Win root checkout $actual_root_commit does not match release manifest $manifest_root_commit"
actual_root_diff="$(git -C "$ORBIS_WIN_ROOT" diff HEAD --binary | sha256sum | awk '{print $1}')"
[[ "$actual_root_diff" == "$empty_diff_sha256" ]] ||
    die "Orbis-Win root checkout has tracked local changes (sha256=$actual_root_diff)"
actual_pw_commit="$(git -C "$ORBIS_WIN_ROOT/prospero-win" rev-parse HEAD)"
[[ "$actual_pw_commit" == "$manifest_commit" ]] ||
    die "Orbis-Win Prosper-Win checkout $actual_pw_commit does not match release manifest $manifest_commit"
actual_pw_diff="$(git -C "$ORBIS_WIN_ROOT/prospero-win" diff HEAD --binary | sha256sum | awk '{print $1}')"
[[ "$actual_pw_diff" == "$empty_diff_sha256" ]] ||
    die "Orbis-Win Prosper-Win checkout has tracked local changes (sha256=$actual_pw_diff)"
if [[ -n "$PW_SOURCE_COMMIT" && "$PW_SOURCE_COMMIT" != "$manifest_commit" ]]; then
    die "PW_SOURCE_COMMIT $PW_SOURCE_COMMIT does not match release manifest commit $manifest_commit"
fi
PW_SOURCE_COMMIT="$manifest_commit"

[[ -d "$ORBIS_WIN_STAGE/sce_module" ]] || die "missing runtime stage: $ORBIS_WIN_STAGE"
[[ -x "$CC" && -f "$SDK/link.x" ]] || die "orbis-win PS4 compiler/SDK missing"
[[ -n "$HOST_WINE_ROOT" ]] || die "release manifest has no host_wine path"
if [[ -n "$WINE_MONO_DIR" ]]; then
    [[ "$WINE_MONO_VERSION" =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?$ ]] || die "invalid WINE_MONO_VERSION: $WINE_MONO_VERSION"
    for path in bin/libmono-2.0-x86.dll bin/libmono-2.0-x86_64.dll etc/mono lib/mono/4.5/mscorlib.dll; do
        [[ -e "$WINE_MONO_DIR/$path" ]] || die "Wine Mono payload is incomplete: $WINE_MONO_DIR/$path"
    done
fi
case "$VKD3D_PROTON_QUALIFIED" in
    0|1) ;;
    *) die "VKD3D_PROTON_QUALIFIED must be 0 or 1" ;;
esac
if [[ -n "$VKD3D_PROTON_DIR" ]]; then
    [[ "$VKD3D_PROTON_VERSION" =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?$ ]] ||
        die "invalid VKD3D_PROTON_VERSION: $VKD3D_PROTON_VERSION"
    for path in x86/d3d12.dll x86/d3d12core.dll x64/d3d12.dll x64/d3d12core.dll; do
        [[ -f "$VKD3D_PROTON_DIR/$path" ]] || die "vkd3d-proton payload is incomplete: $VKD3D_PROTON_DIR/$path"
    done
fi
WINE="$HOST_WINE_ROOT/install/usr/bin/wine"
[[ -x "$WINE" ]] || die "manifest host Wine is missing or not executable: $WINE"
[[ -f "$ORBIS_WIN_STAGE/sce_module/libvulkan.prx" ]] || die "Mesa PRX missing from baseline stage"
[[ -f "$ORBIS_WIN_STAGE/sce_module/relocated_wine_app.prx" ]] ||
    die "released relocated Wine PRX missing from baseline stage"
[[ -f "$ORBIS_WIN_STAGE/win/wine/lib/wine/x86_64-windows/wow64native.dll" ]] ||
    die "wow64native.dll missing from baseline Wine runtime"
expected_mesa="$(awk '$1 == "sha256" && $3 == "sce_module/libvulkan.prx" { print $2 }' "$RELEASE_MANIFEST")"
[[ -n "$expected_mesa" ]] || die "release manifest has no libvulkan.prx hash"
if [[ -n "$MESA_SHA256" && "$MESA_SHA256" != "$expected_mesa" ]]; then
    die "PLAYDEN_MESA_SHA256 $MESA_SHA256 does not match release manifest $expected_mesa"
fi
MESA_SHA256="$expected_mesa"
actual_mesa="$(sha256sum "$ORBIS_WIN_STAGE/sce_module/libvulkan.prx" | awk '{print $1}')"
[[ "$actual_mesa" == "$MESA_SHA256" ]] ||
    die "Mesa driver hash $actual_mesa does not match release manifest $MESA_SHA256"
expected_wine_prx="$(awk '$1 == "sha256" && $3 == "sce_module/relocated_wine_app.prx" { print $2 }' "$RELEASE_MANIFEST")"
[[ -n "$expected_wine_prx" ]] ||
    die "release manifest has no relocated_wine_app.prx hash"
actual_wine_prx="$(sha256sum "$ORBIS_WIN_STAGE/sce_module/relocated_wine_app.prx" | awk '{print $1}')"
[[ "$actual_wine_prx" == "$expected_wine_prx" ]] ||
    die "released Wine PRX hash $actual_wine_prx does not match release manifest $expected_wine_prx"
# Read DXVK's version out of the DLL that is actually being shipped.
dxvk_version() {
    local v
    v=$(strings -a "$1/d3d11.dll" 2>/dev/null | grep -oE "^v[0-9]+\.[0-9]+(\.[0-9]+)?\+?[A-Za-z0-9.]*$" | head -1)
    printf '%s' "${v:-unknown}"
}
DXVK_DLLS="d3d8 d3d9 d3d10core d3d11 dxgi"
pe_machine() {
    python3 -c 'import struct,sys
d=open(sys.argv[1],"rb").read(4096)
o=struct.unpack_from("<I",d,0x3c)[0]
print(hex(struct.unpack_from("<H",d,o+4)[0]) if d[:2]==b"MZ" and d[o:o+4]==b"PE\0\0" else "none")' "$1"
}
for dll in $DXVK_DLLS; do
    [[ -f "$DXVK_262_DIR/$dll.dll" ]] || die "DXVK x32 $dll.dll missing from $DXVK_262_DIR"
    [[ -f "$DXVK_262_X64_DIR/$dll.dll" ]] || die "DXVK x64 $dll.dll missing from $DXVK_262_X64_DIR"
    [[ "$(pe_machine "$DXVK_262_DIR/$dll.dll")" == 0x14c ]] || die "$DXVK_262_DIR/$dll.dll is not 32-bit"
    [[ "$(pe_machine "$DXVK_262_X64_DIR/$dll.dll")" == 0x8664 ]] || die "$DXVK_262_X64_DIR/$dll.dll is not 64-bit"
done
if [[ -n "$VKD3D_PROTON_DIR" ]]; then
    for dll in d3d12.dll d3d12core.dll; do
        [[ "$(pe_machine "$VKD3D_PROTON_DIR/x86/$dll")" == 0x14c ]] ||
            die "$VKD3D_PROTON_DIR/x86/$dll is not 32-bit"
        [[ "$(pe_machine "$VKD3D_PROTON_DIR/x64/$dll")" == 0x8664 ]] ||
            die "$VKD3D_PROTON_DIR/x64/$dll is not 64-bit"
    done
elif [[ "$VKD3D_PROTON_QUALIFIED" == 1 ]]; then
    die "cannot qualify vkd3d-proton without VKD3D_PROTON_DIR"
fi
WINE_VERSION="$("$WINE" --version)"
[[ "$WINE_VERSION" == wine-* ]] || die "manifest host Wine did not report a version: $WINE_VERSION"
MESA_COMMIT="$(sed -n 's/^mesa_commit=//p' "$RELEASE_MANIFEST")"
[[ -n "$MESA_COMMIT" ]] || die "release manifest has no mesa_commit"
MESA_SOURCE_DIFF="$(sed -n 's/^mesa_source_diff_sha256=//p' "$RELEASE_MANIFEST")"
[[ "$MESA_SOURCE_DIFF" == "$empty_diff_sha256" ]] ||
    die "Orbis-Win Mesa source has local changes (mesa_source_diff_sha256=${MESA_SOURCE_DIFF:-missing})"
EXPECTED_MESA_COMMIT="$(tr -d '[:space:]' < "$ORBIS_WIN_ROOT/src/mesa-trees/mesa-ps4-HEAD.txt")"
[[ "$MESA_COMMIT" == "$EXPECTED_MESA_COMMIT" ]] ||
    die "Orbis-Win Mesa commit $MESA_COMMIT does not match pinned commit $EXPECTED_MESA_COMMIT"
STEAMWORKS_DIR="$ROOT/runtime/steamworks"
for spec in \
    'steam_api.dll d778ba3007cc46b7587f23698493c6734752213e34eaec2d5e59a036da11df44' \
    'steam_api64.dll ca4698c8d8db8270735c3570922748300abd10dc9d16c2f3a95a6a564be144d2' \
    'steam.dll f813075716892ab19ee9b0c06000c328bd5f15e713692384a30ddbcfc9065e57'; do
    read -r dll expected <<<"$spec"
    [[ -f "$STEAMWORKS_DIR/$dll" ]] || die "Steamworks emulator file missing: $STEAMWORKS_DIR/$dll"
    actual="$(sha256sum "$STEAMWORKS_DIR/$dll" | awk '{print $1}')"
    [[ "$actual" == "$expected" ]] || die "Steamworks emulator hash mismatch for $dll: $actual"
done

export OO_PS4_TOOLCHAIN="$SDK" ORBIS_COMPAT_DIR="$COMPAT"
cmake -S "$ROOT/ps4" -B "$CMAKE_BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$COMPAT/cmake/ps4-openorbis.cmake" \
    -DOO_PS4_TOOLCHAIN="$SDK" -DORBIS_COMPAT_DIR="$COMPAT" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$CMAKE_BUILD" --target playden_ps4_shell playden_ps4_homebrew_preview playden_ps4_homebrew_live playden_ps4_publication playden_ps4_ui_platform -j"${JOBS:-4}"

mkdir -p "$BUILD/obj" "$BUILD/prx" "$BUILD/stub" "$PKG_OUT"
PRX_BUILD="$BUILD/prx"
PRX_STUBS="$PW/.deps/wine-ps5/prx/stubs"
[[ -d "$PRX_STUBS" ]] || die "Wine PRX import stubs missing: $PRX_STUBS"
export PW_SHARED_LIBC=1 PW_PRX_LINK=1
export PW_PRX_STUBS="-L$PRX_STUBS -Wl,--as-needed $(find "$PRX_STUBS" -maxdepth 1 -name '*.so' -print | tr '\n' ' ')"

echo "== link Playden UI into a relocatable PRX"
"$CC" -std=gnu11 -O2 -Wall -Wextra -Werror -fPIC \
    -c "$ROOT/ps4/runtime_entry.c" -o "$BUILD/obj/runtime_entry.o"
# The UI PRX imports SceLibc malloc/free. Its own exported heap policy is
# required for multi-megabyte Steam manifests; the eboot CRT's local symbols
# are not exported into this separately loaded module.
"$CC" -std=gnu11 -O2 -Wall -Wextra -Werror -fPIC \
    -c "$COMPAT/optional/orbis_bigheap.c" -o "$BUILD/obj/orbis_bigheap.o"
# libSceLibcInternal's heap stops at about 13 MiB on PS4 and does not grow (measured on
# 01.85), so every allocation in the UI module goes to Playden's own heap instead. --wrap
# keeps libc's own free/realloc reachable for memory libc allocated (strdup, realpath).
"$CC" -std=gnu11 -O2 -Wall -Wextra -Werror -fPIC \
    -c "$ROOT/ps4/playden_heap.c" -o "$BUILD/obj/playden_heap.o"
HEAP_WRAPS="-Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free -Wl,--wrap=posix_memalign"
# opendir fails with EPERM for every folder in this title (02.34 probe) while
# sceKernelGetdents lists them, so the module's folder listing, std::filesystem's
# included, goes through ps4/dir_wrap.c.
"$CC" -std=gnu11 -O2 -Wall -Wextra -Werror -fPIC \
    -c "$ROOT/ps4/dir_wrap.c" -o "$BUILD/obj/dir_wrap.o"
HEAP_WRAPS="$HEAP_WRAPS -Wl,--wrap=opendir -Wl,--wrap=fdopendir -Wl,--wrap=readdir -Wl,--wrap=closedir"
# The SDK's stat leaves st_dev/st_ino/st_uid/st_gid zero (sceKernelStat, 02.38 probe), which
# Playden's ManagedDirectory reads as a replaced folder (ESTALE): ps4/stat_wrap.c fills them from fstat.
"$CC" -std=gnu11 -O2 -Wall -Wextra -Werror -fPIC \
    -c "$ROOT/ps4/stat_wrap.c" -o "$BUILD/obj/stat_wrap.o"
HEAP_WRAPS="$HEAP_WRAPS -Wl,--wrap=stat -Wl,--wrap=lstat -Wl,--wrap=fstatat"
"$CC" -shared -Wl,-Bsymbolic -Wl,--eh-frame-hdr -Wl,-soname,playden_ui.prx \
    -Wl,--warn-unresolved-symbols $HEAP_WRAPS -o "$PRX_BUILD/playden_ui.shared.elf" \
    -Wl,-u,playden_ps4_preview_wanted -Wl,-u,playden_ps4_preview_run \
    "$CMAKE_BUILD/CMakeFiles/playden_ps4_app_objects.dir/main.c.obj" \
    "$CMAKE_BUILD/CMakeFiles/playden_ps4_app_objects.dir/wine_runtime.c.obj" \
    "$BUILD/obj/runtime_entry.o" \
    "$BUILD/obj/orbis_bigheap.o" \
    "$BUILD/obj/playden_heap.o" \
    "$BUILD/obj/dir_wrap.o" \
    "$BUILD/obj/stat_wrap.o" \
    "$CMAKE_BUILD/libplayden_ps4_hui.a" \
    -Wl,--start-group \
    "$CMAKE_BUILD/libplayden_ps4_steam_auth.a" \
    "$CMAKE_BUILD/libplayden_ps4_steam_cloud.a" \
    "$CMAKE_BUILD/libplayden_ps4_steam_install.a" \
    "$CMAKE_BUILD/libplayden_ps4_depot_codec.a" \
    "$CMAKE_BUILD/libplayden_ps4_game_settings.a" \
    "$CMAKE_BUILD/libplayden_ps4_miniz.a" \
    "$CMAKE_BUILD/libplayden_ps4_update.a" \
    "$CMAKE_BUILD/libplayden_ps4_homebrew_preview.a" \
    "$CMAKE_BUILD/libplayden_ps4_homebrew_live.a" \
    "$CMAKE_BUILD/libplayden_ps4_publication.a" \
    "$CMAKE_BUILD/libplayden_ps4_ui_platform.a" \
    -Wl,--end-group \
    "$ROOT/scripts/network-deps/.deps/ps4/lib/libcurl.a" \
    "$ROOT/scripts/network-deps/.deps/ps4/lib/libmbedtls.a" \
    "$ROOT/scripts/network-deps/.deps/ps4/lib/libmbedx509.a" \
    "$ROOT/scripts/network-deps/.deps/ps4/lib/libmbedcrypto.a" \
    "$ROOT/scripts/network-deps/.deps/ps4/lib/libeverest.a" \
    "$ROOT/scripts/network-deps/.deps/ps4/lib/libp256m.a" \
    -lSceNet -lSceSysmodule -lSceImeDialog -lSceVideoOut -lSceSystemService \
    -lSceUserService -lScePad -lm \
    >"$PRX_BUILD/playden_ui.link.log" 2>&1 || {
        tail -40 "$PRX_BUILD/playden_ui.link.log" >&2
        die "Playden UI PRX link failed"
    }
# The UI PRX runs its own .init_array at entry (ps4/runtime_entry.c, playden_ps4_start): the system
# loader runs a module's init only through Sony's C runtime, which this module has none of. Check the
# bounds it walks are this module's .init_array, so namespace-scope objects with constructors are fine.
init_array_section="$(readelf -SW "$PRX_BUILD/playden_ui.shared.elf" | sed -E 's/^ *\[ *[0-9]+\] *//' |
    awk '$1 == ".init_array" {print $3, $5}')"
read -r init_array_addr init_array_size <<<"$init_array_section"
init_bounds="$(nm "$PRX_BUILD/playden_ui.shared.elf" | awk '$3 == "__init_array_start" || $3 == "__init_array_end" {print $3, $1}' | sort)"
[[ -n "$init_array_addr" ]] || die "UI PRX has no .init_array"
init_start="$(awk '$1 == "__init_array_start" {print $2}' <<<"$init_bounds")"
init_end="$(awk '$1 == "__init_array_end" {print $2}' <<<"$init_bounds")"
[[ -n "$init_start" && -n "$init_end" ]] || die "UI PRX does not define __init_array_start/__init_array_end"
(( 16#$init_start == 16#$init_array_addr && 16#$init_end == 16#$init_array_addr + 16#$init_array_size )) ||
    die "UI PRX init_array bounds $init_start-$init_end do not match .init_array $init_array_addr+$init_array_size"
heap_exports="$(nm -D "$PRX_BUILD/playden_ui.shared.elf")"
# Nothing in the module may still call libc's allocator directly, except the heap's own
# fallback for memory libc allocated.
direct_alloc="$(nm "$PRX_BUILD/playden_ui.shared.elf" | awk '$1 == "U" && $2 ~ /^(malloc|calloc|realloc|posix_memalign|aligned_alloc|memalign)$/ {print $2}')"
[[ "$direct_alloc" == "realloc" || -z "$direct_alloc" ]] ||
    die "UI PRX still imports libc allocators directly: $direct_alloc"
direct_dir="$(nm "$PRX_BUILD/playden_ui.shared.elf" | awk '$1 == "U" && $2 ~ /^(opendir|fdopendir|readdir|closedir)$/ {print $2}' | tr '\n' ' ')"
[[ -z "$direct_dir" ]] || die "UI PRX still imports libc directory calls directly: $direct_dir"
for heap_symbol in sceLibcHeapSize sceLibcHeapExtendedAlloc; do
    grep -Eq " [DB] ${heap_symbol}\$" <<<"$heap_exports" ||
        die "UI PRX does not export $heap_symbol"
done
"$ORBIS_WIN_ROOT/scripts/create-fself-repro.sh" "$SDK/bin/linux/create-fself" \
    -in "$PRX_BUILD/playden_ui.shared.elf" -out "$PRX_BUILD/playden_ui.oelf" \
    -lib "$PRX_BUILD/playden_ui.prx" -libname playden_ui.prx -ptype fake \
    -library-path "$PRX_STUBS:$ORBIS_WIN_ROOT/build/ps4-wine-sdk:$SDK/lib" \
    >"$PRX_BUILD/playden_ui.create-fself.log" 2>&1 || {
        tail -30 "$PRX_BUILD/playden_ui.create-fself.log" >&2
        die "could not convert Playden UI into a PRX"
    }
python3 "$ROOT/scripts/check-prx-bss-coverage.py" \
    "$PRX_BUILD/playden_ui.shared.elf" "$PRX_BUILD/playden_ui.oelf" ||
    die "Playden UI .bss is not covered by the converted module"

echo "== use the released Orbis-Win runtime PRX unchanged"
cp "$ORBIS_WIN_STAGE/sce_module/relocated_wine_app.prx" \
    "$PRX_BUILD/relocated_wine_app.prx"

echo "== create the low-address Playden eboot handoff"
PW_EXECUTABLE_LD_SCRIPT="$ORBIS_WIN_ROOT/src/reloc-wine-app/stub-no-relro.ld" \
PW_EXECUTABLE_NO_RELRO=1 PW_EXECUTABLE_NO_ORBIS_COMPAT=1 \
    "$CC" -std=gnu11 -O2 -Wall -Wextra -Werror -fPIC \
    "$ROOT/ps4/runtime_stub.c" "$BUILD/obj/orbis_bigheap.o" -o "$BUILD/stub/stub-linked.elf" \
    -lSceSystemService -lSceSysmodule -lSceVideoOut -lScePad -lSceUserService
python3 "$ORBIS_WIN_ROOT/scripts/prepare-orbis-stub-no-relro.py" \
    --layout 0x55c000 0x7fff0000 0x80000000 \
    "$BUILD/stub/stub-linked.elf" "$BUILD/stub/stub.elf"
"$ORBIS_WIN_ROOT/scripts/create-fself-repro.sh" "$SDK/bin/linux/create-fself" \
    -in "$BUILD/stub/stub.elf" -out "$BUILD/stub/stub.oelf" -eboot "$BUILD/eboot.bin" \
    >"$BUILD/stub/create-fself.log" 2>&1 || {
        cat "$BUILD/stub/create-fself.log" >&2
        die "could not create the Playden low-address eboot"
    }

echo "== prepare a clean Wine prefix with the matching host build"
PREFIX="$BUILD/prefix-template/hl2-prefix"
rm -rf "$PREFIX"
mkdir -p "$PREFIX/drive_c/Games"
USER=prospero LOGNAME=prospero WINEARCH=win64 WINEPREFIX="$PREFIX" WINEDEBUG=-all \
    WINEDLLOVERRIDES='winemenubuilder.exe=d;mshtml=;mscoree=' \
    "$WINE" wineboot --init
# The PS4 kit view advertises PS5 audio when wineps5.prx is staged. Recipes such as HL2's
# require the matching Wine driver selection in the template bottle too; the game bottles are
# cloned from this prefix and recipe requirements are admission checks, not per-game registry
# mutations. Keep the capability and the template registry in sync.
USER=prospero LOGNAME=prospero WINEARCH=win64 WINEPREFIX="$PREFIX" WINEDEBUG=-all \
    "$WINE" reg add 'HKCU\Software\Wine\Drivers' /v Audio /t REG_SZ /d ps5 /f
if [[ -n "$WINE_MONO_DIR" ]]; then
    USER=prospero LOGNAME=prospero WINEARCH=win64 WINEPREFIX="$PREFIX" WINEDEBUG=-all \
        "$WINE" reg add 'HKCU\Software\Wine\Mono' /v RuntimePath /t REG_SZ \
        /d "Z:\\data\\playden\\kits\\$KIT_ID\\mono\\wine-mono-$WINE_MONO_VERSION" /f
fi
for dll in $DXVK_DLLS; do
    cp "$DXVK_262_DIR/$dll.dll" "$PREFIX/drive_c/windows/syswow64/$dll.dll"
    cp "$DXVK_262_X64_DIR/$dll.dll" "$PREFIX/drive_c/windows/system32/$dll.dll"
done
WINEPREFIX="$PREFIX" WINEDEBUG=-all "$WINE" wineboot --shutdown
# The registry is written when wineserver exits; wait for it before copying the prefix.
WINEPREFIX="$PREFIX" "$HOST_WINE_ROOT/install/usr/bin/wineserver" -w
for attempt in $(seq 1 10); do
    [[ -s "$PREFIX/system.reg" && -s "$PREFIX/drive_c/windows/syswow64/d3d9.dll" ]] && break
    sleep 1
done
[[ -s "$PREFIX/system.reg" && -s "$PREFIX/drive_c/windows/syswow64/d3d9.dll" ]] || {
    ls -l "$PREFIX/system.reg" "$PREFIX/drive_c/windows/syswow64/d3d9.dll" >&2 || true
    die "HL2 Wine prefix template is incomplete"
}
# wineboot stamps every registry key with the wall clock and invents per-boot GUIDs, so the kit's
# files_sha256 (which covers system.reg and user.reg) changed on every build. That hash goes into
# every prepared game's owner marker, so TitleCurrent() was false for all of them and Playden
# offered "Update games to the new runtime kit" after every package install, forever, with Play
# gated behind it. Canonicalizing makes two builds of the same runtime produce the same kit.
python3 "$ROOT/scripts/canonicalize-prefix-registry.py" "$PREFIX"
for dll in $DXVK_DLLS; do
    cmp -s "$DXVK_262_X64_DIR/$dll.dll" "$PREFIX/drive_c/windows/system32/$dll.dll" ||
        die "Wine replaced DXVK's x64 $dll.dll in the template"
    cmp -s "$DXVK_262_DIR/$dll.dll" "$PREFIX/drive_c/windows/syswow64/$dll.dll" ||
        die "Wine replaced DXVK's x32 $dll.dll in the template"
done

echo "== assemble PLDN10001 with Orbis-Win's tested Mesa build"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -a "$ORBIS_WIN_STAGE/." "$STAGE/"
cp "$BUILD/eboot.bin" "$STAGE/eboot.bin"
cp "$PRX_BUILD/playden_ui.prx" "$STAGE/sce_module/playden_ui.prx"
cp "$PRX_BUILD/relocated_wine_app.prx" "$STAGE/sce_module/relocated_wine_app.prx"
cmp -s "$ORBIS_WIN_STAGE/sce_module/relocated_wine_app.prx" \
    "$STAGE/sce_module/relocated_wine_app.prx" ||
    die "packaged Wine runtime differs from the released Orbis-Win PRX"
mkdir -p "$STAGE/runtime-template/hl2-prefix" "$STAGE/assets" "$STAGE/runtime/steamworks"
if [[ -n "$WINE_MONO_DIR" ]]; then
    MONO_STAGE="$STAGE/runtime-template/kit/mono/wine-mono-$WINE_MONO_VERSION"
    mkdir -p "$MONO_STAGE"
    for path in bin etc lib support; do
        [[ ! -e "$WINE_MONO_DIR/$path" ]] || cp -a "$WINE_MONO_DIR/$path" "$MONO_STAGE/"
    done
fi
if [[ -n "$VKD3D_PROTON_DIR" ]]; then
    for spec in 'x64 x86_64-windows' 'x86 i386-windows'; do
        read -r source_arch target_arch <<<"$spec"
        destination="$STAGE/runtime-template/kit/runtime/lib/vkd3d-proton/$target_arch"
        mkdir -p "$destination"
        cp "$VKD3D_PROTON_DIR/$source_arch/d3d12.dll" \
           "$VKD3D_PROTON_DIR/$source_arch/d3d12core.dll" "$destination/"
    done
fi
cp "$ORBIS_WIN_STAGE/win/wine/lib/wine/x86_64-windows/wow64native.dll" \
    "$PREFIX/drive_c/windows/system32/wow64native.dll"
cp -a "$PREFIX/." "$STAGE/runtime-template/hl2-prefix/"
# Identifies the Wine-owned part of the template. A prefix made from an older template is
# refreshed (drive_c/windows only; registry and user files are kept) when this changes.
TEMPLATE_STAMP="$( {
    printf '%s\n' "$WINE_VERSION"
    (cd "$PREFIX" && find drive_c/windows -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum)
} | sha256sum | cut -c1-64)"
printf '%s\n' "$TEMPLATE_STAMP" >"$STAGE/runtime-template/hl2-prefix.stamp"
{
    find "$PREFIX" -mindepth 1 -type d -printf 'D %P\n'
    find "$PREFIX" -type f -printf 'F %m %P\n'
} | LC_ALL=C sort >"$STAGE/runtime-template/hl2-prefix.manifest"
mkdir -p "$STAGE/runtime/input"
cp "$ROOT/runtime/input/"*.input "$STAGE/runtime/input/"
cp "$ROOT/third_party/ps5-homebrew-ui/assets/fonts/inter-regular.huifont" "$STAGE/assets/inter-regular.huifont"
# PS4_DEV_CONF=<host>[:<port>] stages /app0/dev.conf, which the bundled Orbis-Win reads at the top of
# main() (before the /data grant, when its own settings.conf is not readable yet) to send its log to
# that PC. Test builds only; a release build leaves it out, as Orbis-Win's own packager does.
if [[ -n "${PS4_DEV_CONF:-}" ]]; then
    printf 'DEV_ENABLED=1\nDEV_SERVER=%s\nDEV_PORT=%s\n' "${PS4_DEV_CONF%%:*}" \
        "$([[ $PS4_DEV_CONF == *:* ]] && printf '%s' "${PS4_DEV_CONF##*:}" || printf 9300)" >"$STAGE/dev.conf"
    echo "== staged /app0/dev.conf -> ${PS4_DEV_CONF}"
fi
# Playden's own UI for the CPU-renderer preview (ps4/homebrew_preview.cpp): its simulation catalog,
# fixture art, glyphs, presets and the Barlow/Noto atlases, baked as the desktop build bakes them.
HUI_BAKER="$BUILD/host/hui-bake-font"
mkdir -p "$BUILD/host" "$BUILD/homebrew-fonts"
[[ -x "$HUI_BAKER" ]] || g++ -std=c++20 -O2 -I"$ROOT/third_party/ps5-homebrew-ui/third_party/stb" \
    "$ROOT/third_party/ps5-homebrew-ui/tools/font-baker/bake_font.cpp" -o "$HUI_BAKER"
FIXTURE_FONTS="$ROOT/fixtures/ui-comparison/assets/fonts"
FIXTURE_CATALOG="$ROOT/fixtures/ui-comparison/catalog.json"
bake() { [[ -s "$BUILD/homebrew-fonts/$2" ]] || "$HUI_BAKER" "$1" "$BUILD/homebrew-fonts/$2" 56 8 2048 "$3" >/dev/null 2>&1 ||
    die "could not bake $2"; }
bake "$FIXTURE_FONTS/Barlow-Regular.ttf" barlow-regular.huifont european
bake "$FIXTURE_FONTS/Barlow-Medium.ttf" barlow-medium.huifont european
bake "$FIXTURE_FONTS/Barlow-SemiBold.ttf" barlow-semibold.huifont european
bake "$FIXTURE_FONTS/BarlowCondensed-Medium.ttf" barlow-condensed-medium.huifont european
bake "$FIXTURE_FONTS/BarlowCondensed-SemiBold.ttf" barlow-condensed-semibold.huifont european
bake "$FIXTURE_FONTS/NotoSansCJKsc-Regular.otf" noto-cjk-fallback.huifont "european,text=$FIXTURE_CATALOG"
bake "$FIXTURE_FONTS/NotoSansArabic.ttf" noto-arabic-fallback.huifont "basic,text=$FIXTURE_CATALOG"
bake "$ROOT/assets/fonts/NotoSans-Regular.ttf" noto-greek-fallback.huifont "european,text=$FIXTURE_CATALOG"
mkdir -p "$STAGE/homebrew/assets" "$STAGE/homebrew/fixtures"
cp -r "$ROOT/fixtures/ui-comparison/assets/art" "$ROOT/fixtures/ui-comparison/assets/glyphs" "$STAGE/homebrew/assets/"
mkdir -p "$STAGE/homebrew/assets/fonts"
cp "$BUILD"/homebrew-fonts/*.huifont "$STAGE/homebrew/assets/fonts/"
cp "$FIXTURE_FONTS"/*.txt "$ROOT/assets/fonts/NotoSans-OFL.txt" "$STAGE/homebrew/assets/fonts/"
cp -r "$ROOT/runtime" "$STAGE/homebrew/assets/runtime"
cp "$FIXTURE_CATALOG" "$ROOT/fixtures/ui-comparison/scenarios.json" "$STAGE/homebrew/fixtures/"
cp "$ROOT/scripts/network-deps/.deps/ps4/cacert.pem" "$STAGE/assets/cacert.pem"
# The kit's identity, not its contents. It must NOT move with the runtime: a marker's kit_id is
# part of a bottle's identity (RespinIdentity in src/runtime/package_installer.cpp), while
# files_sha256, runtime_revision, runtime_path and variant_id are deliberately excluded from it so
# they can change. Minting "orbis-win-ps4-<commit>-<template>" meant every runtime build orphaned
# every installed game: "installed title and bottle are not this install's from a compatible kit",
# and Update runtime kit refused with the message it was supposed to fix, leaving Recreate bottle
# -- which discards the prefix -- as the only path. What actually changed still reaches the console
# through the kit view: runtime.artifact_revision carries $PW_SOURCE_COMMIT, files_sha256 covers
# the template, and capabilities say what the runtime can do.
python3 "$ROOT/scripts/ps4-kit-view.py" "$STAGE" "$KIT_ID" \
    "$PW_SOURCE_COMMIT" "$WINE_VERSION"
cp "$STEAMWORKS_DIR/steam_api.dll" "$STEAMWORKS_DIR/steam_api64.dll" \
    "$STEAMWORKS_DIR/steam.dll" "$STEAMWORKS_DIR/LICENSE" "$STEAMWORKS_DIR/README.md" \
    "$STAGE/runtime/steamworks/"
MONO_TREE_SHA256=""
if [[ -n "$WINE_MONO_DIR" ]]; then
    MONO_TREE_SHA256="$(cd "$WINE_MONO_DIR" && find . -mindepth 1 -type f ! -name extract.log -print0 | LC_ALL=C sort -z | xargs -0 sha256sum | sha256sum | cut -d' ' -f1)"
fi
VKD3D_PROTON_TREE_SHA256=""
if [[ -n "$VKD3D_PROTON_DIR" ]]; then
    VKD3D_PROTON_TREE_SHA256="$(cd "$VKD3D_PROTON_DIR" && sha256sum x86/d3d12.dll x86/d3d12core.dll x64/d3d12.dll x64/d3d12core.dll | sha256sum | cut -d' ' -f1)"
fi
printf 'title=PLDN10001\nversion=%s\norbis_win_root_commit=%s\norbis_win_root_diff_sha256=%s\nprospero_win_abi4_commit=%s\nprospero_win_abi4_diff_sha256=%s\nwine=%s\nmesa_commit=%s\nmesa_source_diff_sha256=%s\nmesa_libvulkan_sha256=%s\ndxvk=%s-x32+x64\ndxvk_d3d11_sha256=%s\nprefix_template_stamp=%s\nwine_mono_version=%s\nwine_mono_tree_sha256=%s\nvkd3d_proton_version=%s\nvkd3d_proton_tree_sha256=%s\nvkd3d_proton_qualified=%s\nsteamworks_emulator=gbe_fork-ca45922c\n' \
    "$PKG_VERSION" "$manifest_root_commit" "$manifest_root_diff" "$PW_SOURCE_COMMIT" "$manifest_diff" "${WINE_VERSION#wine-}" \
    "$MESA_COMMIT" "$MESA_SOURCE_DIFF" "$MESA_SHA256" "$(dxvk_version "$DXVK_262_DIR")" \
    "$(sha256sum "$DXVK_262_X64_DIR/d3d11.dll" | cut -d" " -f1)" "$TEMPLATE_STAMP" \
    "$WINE_MONO_VERSION" "$MONO_TREE_SHA256" "$VKD3D_PROTON_VERSION" "$VKD3D_PROTON_TREE_SHA256" \
    "$VKD3D_PROTON_QUALIFIED" >"$STAGE/runtime-manifest.txt"
cmp -s "$ORBIS_WIN_STAGE/sce_module/libvulkan.prx" "$STAGE/sce_module/libvulkan.prx" || die "packaged Mesa PRX mismatch"
cmp -s "$STAGE/sce_module/libvulkan.prx" "$STAGE/win/wine/lib/wine/x86_64-unix/libvulkan.prx" ||
    die "Wine's Vulkan loader and system Vulkan module differ"

ICON="$ROOT/assets/branding/generic-title-icon.png"
"$COMPAT/scripts/ps4/make-pkg.sh" --eboot "$BUILD/eboot.bin" --out-dir "$PKG_OUT" \
    --icon "$ICON" --sdk "$SDK" --title-id "$TITLE_ID" --title "Playden for PS4" \
    --version "$PKG_VERSION"

# make-pkg's --extra interface passes every filename as one argv entry to create-gp4; the
# Orbis-Win Wine tree exceeds Linux's per-argument limit. Reuse its SFO creation, merge our
# complete runtime tree into that stage, then author the GP4 file table directly.
FINAL_STAGE="$PKG_OUT/pkg-stage"
cp "$FINAL_STAGE/sce_sys/param.sfo" "$BUILD/param.sfo"
cp "$FINAL_STAGE/sce_sys/icon0.png" "$BUILD/icon0.png"
cp -a "$STAGE/." "$FINAL_STAGE/"
cp "$BUILD/param.sfo" "$FINAL_STAGE/sce_sys/param.sfo"
cp "$BUILD/icon0.png" "$FINAL_STAGE/sce_sys/icon0.png"
rm -f "$FINAL_STAGE/pkg.gp4" "$FINAL_STAGE"/*.pkg "$FINAL_STAGE/pkg-build.log"
(
    cd "$FINAL_STAGE"
    "$SDK/bin/linux/create-gp4" -out pkg.gp4 --content-id="IV0000-${TITLE_ID}_00-PLDN100010000000" \
        --files "eboot.bin sce_sys/param.sfo" >/dev/null
    python3 - "$FINAL_STAGE/pkg.gp4" "$FINAL_STAGE" "IV0000-${TITLE_ID}_00-PLDN100010000000" <<'PY'
import os, sys, xml.etree.ElementTree as ET
gp4, stage, content_id = sys.argv[1:]
tree = ET.parse(gp4)
root = tree.getroot()
root.find("./volume/package").set("content_id", content_id)
files = root.find("files")
for child in list(files):
    files.remove(child)
rootdir = root.find("rootdir")
for child in list(rootdir):
    rootdir.remove(child)
directories = {}
paths = []
for base, dirs, names in os.walk(stage):
    dirs[:] = sorted(d for d in dirs if not os.path.islink(os.path.join(base, d)))
    for name in sorted(names):
        path = os.path.join(base, name)
        if os.path.islink(path) or not os.path.isfile(path):
            continue
        rel = os.path.relpath(path, stage).replace(os.sep, "/")
        if rel in ("pkg.gp4", "pkg-build.log") or rel.endswith(".pkg"):
            continue
        paths.append(rel)
        cursor = directories
        for part in rel.split("/")[:-1]:
            cursor = cursor.setdefault(part, {})
for rel in sorted(paths):
    ET.SubElement(files, "file", targ_path=rel, orig_path=rel)
def emit(parent, children):
    for name in sorted(children):
        node = ET.SubElement(parent, "dir", targ_name=name)
        emit(node, children[name])
emit(rootdir, directories)
tree.write(gp4, encoding="utf-8", xml_declaration=True)
print(f"GP4 contains {len(paths)} files")
PY
    DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 "$SDK/bin/linux/PkgTool.Core" pkg_build pkg.gp4 . >pkg-build.log 2>&1 || {
        tail -50 pkg-build.log >&2
        exit 1
    }
)
PKG_FILE="$FINAL_STAGE/IV0000-${TITLE_ID}_00-PLDN100010000000.pkg"
[[ -s "$PKG_FILE" ]] || die "PkgTool.Core produced no package"
mv -f "$PKG_FILE" "$PKG_OUT/IV0000-${TITLE_ID}_00-PLDN100010000000.pkg"
ln -sf "IV0000-${TITLE_ID}_00-PLDN100010000000.pkg" "$PKG_OUT/${TITLE_ID}.pkg"
PKG_FILE="$PKG_OUT/IV0000-${TITLE_ID}_00-PLDN100010000000.pkg"
echo "runtime package: $PKG_FILE ($(du -h "$PKG_FILE" | cut -f1))"
sha256sum "$PKG_FILE"
echo "Mesa PRX: $MESA_SHA256"
