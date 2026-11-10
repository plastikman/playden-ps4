# OpenOrbis package module stubs

These two modules replace the console-extracted `sce_module/libc.prx` and
`sce_module/libSceFios2.prx` files in experimental/public packages. They are
the empty package stubs from OpenOrbis, built locally with this repository's
pinned OpenOrbis SDK. The module bodies each define only the upstream dummy
function; they do not reimplement libc or FIOS2.

Source origin: `OpenOrbis/OpenOrbis-PS4-Toolchain`, commit
`0a1aaf9dd4a92695538bdeb09fb056d06dd11725`, under GPL-3.0. `COPYING` contains
the upstream license. Keep these sources and the license available with any
binary package that includes the generated modules.

Build with `scripts/build-openorbis-module-stubs.sh`. The outputs are written
to `build/openorbis-module-stubs/` and can be selected for packaging by setting
`PW_MODULE_STUBS_DIR` to that directory. The normal package default continues
to use the local `sce_module/` inputs until these replacements pass hardware
validation.
