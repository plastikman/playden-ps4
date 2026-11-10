#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
ORBIS_WIN_ROOT="${ORBIS_WIN_ROOT:?set ORBIS_WIN_ROOT to the hardware-tested orbis-win checkout}"
ORBIS_WIN_RELEASE="${ORBIS_WIN_RELEASE:?set ORBIS_WIN_RELEASE to the selected Orbis-Win release stage}"
ORBIS_WIN_STAGE="${ORBIS_WIN_STAGE:-$ORBIS_WIN_RELEASE/app/pkg-stage}"
DXVK_262_DIR="${DXVK_262_DIR:?set DXVK_262_DIR to DXVK 2.6.2 x32 DLLs}"
# 64-bit games load D3D from system32, so the template needs DXVK's x64 DLLs there too.
DXVK_262_X64_DIR="${DXVK_262_X64_DIR:-$(dirname "$DXVK_262_DIR")/x64}"
PW="$ORBIS_WIN_ROOT/prospero-win"
SDK="$ORBIS_WIN_ROOT/sdk/orbis-sdk-v1/sdk"
COMPAT="$ORBIS_WIN_ROOT/sdk/orbis-sdk-v1/orbis-compat"
CC="$ORBIS_WIN_ROOT/build/ps4-wine-sdk/bin/prospero-clang"
BUILD="$ROOT/build/ps4-runtime"
CMAKE_BUILD="$BUILD/cmake"
STAGE="$BUILD/pkg-stage"
PKG_OUT="$BUILD/package"
TITLE_ID=PLDN10001
PKG_VERSION="${PKG_VERSION:-01.24}"
MESA_SHA256="${PLAYDEN_MESA_SHA256:-}"
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
manifest_commit="$(sed -n 's/^prospero_win_abi4_commit=//p' "$RELEASE_MANIFEST")"
[[ -n "$manifest_commit" ]] || die "missing exact Orbis-Win source commit from BUILD-MANIFEST.txt"
manifest_root_commit="$(sed -n 's/^root_commit=//p' "$RELEASE_MANIFEST")"
[[ -n "$manifest_root_commit" ]] || die "missing exact Orbis-Win root commit from BUILD-MANIFEST.txt"
if [[ -n "$PW_SOURCE_COMMIT" && "$PW_SOURCE_COMMIT" != "$manifest_commit" ]]; then
    die "PW_SOURCE_COMMIT $PW_SOURCE_COMMIT does not match release manifest commit $manifest_commit"
fi
PW_SOURCE_COMMIT="$manifest_commit"

[[ -d "$ORBIS_WIN_STAGE/sce_module" ]] || die "missing runtime stage: $ORBIS_WIN_STAGE"
[[ -x "$CC" && -f "$SDK/link.x" ]] || die "orbis-win PS4 compiler/SDK missing"
[[ -n "$HOST_WINE_ROOT" ]] || die "release manifest has no host_wine path"
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
DXVK_DLLS="d3d8 d3d9 d3d10core d3d11 dxgi"
pe_machine() {
    python3 -c 'import struct,sys
d=open(sys.argv[1],"rb").read(4096)
o=struct.unpack_from("<I",d,0x3c)[0]
print(hex(struct.unpack_from("<H",d,o+4)[0]) if d[:2]==b"MZ" and d[o:o+4]==b"PE\0\0" else "none")' "$1"
}
for dll in $DXVK_DLLS; do
    [[ -f "$DXVK_262_DIR/$dll.dll" ]] || die "DXVK 2.6.2 x32 $dll.dll missing from $DXVK_262_DIR"
    [[ -f "$DXVK_262_X64_DIR/$dll.dll" ]] || die "DXVK 2.6.2 x64 $dll.dll missing from $DXVK_262_X64_DIR"
    [[ "$(pe_machine "$DXVK_262_DIR/$dll.dll")" == 0x14c ]] || die "$DXVK_262_DIR/$dll.dll is not 32-bit"
    [[ "$(pe_machine "$DXVK_262_X64_DIR/$dll.dll")" == 0x8664 ]] || die "$DXVK_262_X64_DIR/$dll.dll is not 64-bit"
done
WINE_VERSION="$("$WINE" --version)"
[[ "$WINE_VERSION" == wine-* ]] || die "manifest host Wine did not report a version: $WINE_VERSION"
MESA_COMMIT="$(sed -n 's/^mesa_commit=//p' "$RELEASE_MANIFEST")"
[[ -n "$MESA_COMMIT" ]] || die "release manifest has no mesa_commit"
STEAMWORKS_DIR="$ROOT/runtime/steamworks"
for spec in \
    'steam_api.dll cae4372bceda997d6c73cebc1c14eb27d39b74a7999d8f28384bdfd8d746a79c' \
    'steam_api64.dll 70d9826a402d3a78b0c07e9f7ea7c6832468a1113013f31ea570fccaa5445ea9' \
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
cmake --build "$CMAKE_BUILD" --target playden_ps4_shell -j"${JOBS:-4}"

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
"$CC" -shared -Wl,-Bsymbolic -Wl,--eh-frame-hdr -Wl,-soname,playden_ui.prx \
    -Wl,--warn-unresolved-symbols $HEAP_WRAPS -o "$PRX_BUILD/playden_ui.shared.elf" \
    "$CMAKE_BUILD/CMakeFiles/playden_ps4_app_objects.dir/main.c.obj" \
    "$CMAKE_BUILD/CMakeFiles/playden_ps4_app_objects.dir/wine_runtime.c.obj" \
    "$BUILD/obj/runtime_entry.o" \
    "$BUILD/obj/orbis_bigheap.o" \
    "$BUILD/obj/playden_heap.o" \
    "$CMAKE_BUILD/libplayden_ps4_hui.a" \
    -Wl,--start-group \
    "$CMAKE_BUILD/libplayden_ps4_steam_auth.a" \
    "$CMAKE_BUILD/libplayden_ps4_steam_cloud.a" \
    "$CMAKE_BUILD/libplayden_ps4_steam_install.a" \
    "$CMAKE_BUILD/libplayden_ps4_depot_codec.a" \
    "$CMAKE_BUILD/libplayden_ps4_game_settings.a" \
    "$CMAKE_BUILD/libplayden_ps4_miniz.a" \
    "$CMAKE_BUILD/libplayden_ps4_update.a" \
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
# The UI PRX is entered through its descriptor (ps4/runtime_entry.c); its .init_array does not
# run, so a namespace-scope object with a constructor stays all-zero (a std::map crashed 01.91).
# These four only hold objects that are valid when zeroed; anything new must use function-local
# statics instead.
static_inits="$(nm "$PRX_BUILD/playden_ui.shared.elf" | awk '$3 ~ /^_GLOBAL__sub_I_/ {sub(/^_GLOBAL__sub_I_/, "", $3); print $3}' |
    grep -vxE 'cm_client\.cpp|glyphs\.cpp|orbis_paths\.cpp|steam_auth\.cpp' || true)"
[[ -z "$static_inits" ]] ||
    die "UI PRX has static initializers that never run: $static_inits (use function-local statics)"
heap_exports="$(nm -D "$PRX_BUILD/playden_ui.shared.elf")"
# Nothing in the module may still call libc's allocator directly, except the heap's own
# fallback for memory libc allocated.
direct_alloc="$(nm "$PRX_BUILD/playden_ui.shared.elf" | awk '$1 == "U" && $2 ~ /^(malloc|calloc|realloc|posix_memalign|aligned_alloc|memalign)$/ {print $2}')"
[[ "$direct_alloc" == "realloc" || -z "$direct_alloc" ]] ||
    die "UI PRX still imports libc allocators directly: $direct_alloc"
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
python3 "$ROOT/deps/orbis-win-reference/scripts/prepare-orbis-stub-no-relro.py" \
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
cp "$ROOT/scripts/network-deps/.deps/ps4/cacert.pem" "$STAGE/assets/cacert.pem"
cp "$STEAMWORKS_DIR/steam_api.dll" "$STEAMWORKS_DIR/steam_api64.dll" \
    "$STEAMWORKS_DIR/steam.dll" "$STEAMWORKS_DIR/LICENSE" "$STEAMWORKS_DIR/README.md" \
    "$STAGE/runtime/steamworks/"
printf 'title=PLDN10001\nversion=%s\norbis_win_root_commit=%s\nprospero_win_abi4_commit=%s\nprospero_win_abi4_diff_sha256=%s\nwine=%s\nmesa_commit=%s\nmesa_libvulkan_sha256=%s\ndxvk=2.6.2-x32+x64\nprefix_template_stamp=%s\nsteamworks_emulator=gbe_fork-release-2026_09_27\n' \
    "$PKG_VERSION" "$manifest_root_commit" "$PW_SOURCE_COMMIT" "$manifest_diff" "${WINE_VERSION#wine-}" \
    "$MESA_COMMIT" "$MESA_SHA256" "$TEMPLATE_STAMP" >"$STAGE/runtime-manifest.txt"
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
