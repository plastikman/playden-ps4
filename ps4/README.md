# PS4 native shell

`PLDN10001` is the Playden PS4 title ID. The launcher uses PS4 VideoOut and
DualShock 4 input through the PS4 Pad API. It includes Steam sign-in, library
metadata and artwork, managed game downloads and deletion, Steam Cloud saves,
runtime settings, and a Wine launch handoff for supported games. Installed
games, Wine prefixes, saves, and settings are owned by Playden under
`/data/PLDN10001`.

The OpenOrbis bundle is the paired `orbis-sdk-v1` release from
`orbis-ports/orbis-porting-kit`. Download `orbis-sdk-v1.tar.gz` and its
`.sha256` into `deps/`, check with `sha256sum -c`, unpack, then run
`deps/orbis-sdk-v1/verify.sh deps/orbis-sdk-v1`. `deps/` is ignored.

Run `scripts/build-ps4-shell.sh --pkg` from the repository root. This builds
the eboot, two GPL OpenOrbis module stubs, and a package at
`build/ps4-shell/package/IV0000-PLDN10001_00-PLDN100010000000.pkg`.
The module stub sources in `ps4/module-stubs/` are from OpenOrbis via
`plastikman/orbis-win` and retain their GPL-3.0 text in `COPYING`. They define
only dummy entry points; they contain no console-extracted firmware code.

The PS4 Hui controller adapter compiles and follows the field-by-field
translation in orbis-win's `pw_pad_ps4.c`. The stable launcher uses PS4
VideoOut directly. An earlier experimental build linked and called Mesa EGL
and crashed, but its changed link graph means that result does not identify
EGL as the cause. That path has been removed from PLDN10001.

For graphics bring-up, `scripts/build-ps4-vulkan-probe.sh` packages the
paired SDK's documented Vulkan triangle under `PLDN10001`, replacing the
current iteration while the working shell remains available in
`build/ps4-shell/package/`. It uses the
bundle's `vkloader` and RADV link setup, which matches orbis-win's
`scripts/radv-prx-ps4.sh` dependency set. Test this separately before choosing
the renderer backend for Playden's UI.
