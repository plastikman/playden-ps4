# Playden PS4 v0.4.0 release handoff — 2026-11-20

## Current state

The release work is on branch `codex/ps4-v0.4.0-release`, currently at
`ae198ba5` (one local commit ahead of the pushed branch). It is rebased on
current `origin/main`. The branch sets `ps4/VERSION` to `0.4.0`, package
APP_VER to `02.99`, and pins Prosper-Win to published commit
`eaccec6d3382fa4ab61e35d8cc72908aa67ac164`.

Playden v0.4.0 targets Orbis-Win v0.0.8, package 02.98. Because that release
does not publish a build manifest, the Playden workflow uses
`scripts/ci/orbis-win-v0.0.8.lock` to bind the source tag, root commit,
installer filename, SHA-256, app version, and runtime build marker. The
runtime kit records v0.0.8 as its artifact revision and labels unpublished
source-level provenance as unavailable. Local package extraction verified the
locked digest and build marker.

The package workflow runs `PkgTool.Core pkg_validate` and
`check-ps4-package-parity.py` before artifact upload. PkgTool needs both
`DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1` and the cached OpenSSL 1.1 directory
on `LD_LIBRARY_PATH`; both are set in `scripts/ci/build-package.sh`.

## 1012 investigation and gate correction

Hosted run `44756310013` reported that the `kernelbase` no-debugger path did
not call `log_exception_context32`. This was a bad source-root argument, not a
missing Orbis-Win fix. The checker had been pointed at
`ORBIS_WIN_ROOT/prospero-win`, a separate cached Wine tree. The package is
built from the ABI-v4 cache under `PW_ABI4_ROOT/source`; its
`.deps/wine-host/source/dlls/kernelbase/debug.c` contains the 1012 call.

Disassembly of the v0.0.8 package's 32-bit `kernelbase.dll` confirms the
no-debugger path calls `log_exception_context32` at `0x7b066bb8`. The package
already carried the fix, so no new Orbis-Win release is needed. The Playden
parity command now passes `--upstream-root "$PW_ABI4_ROOT/source"` and fails
early if the runner did not set `PW_ABI4_ROOT`.

## Playden 02.99 build result

A local rebuild from this branch passed PkgTool validation and parity with
**0 hard failures**. The parity report still warns that matching
`ntdll.shared.elf` and `wineserver.shared.elf` are unavailable, so those
export/signal-mask checks are skipped. The package uses the locked v0.0.8
Orbis-Win package.

Artifacts are under
`/home/zpeirce/ps4-package-scratch/playden040-gatefix/`:

- Installer: `IV0000-PLDN10001_00-PLDN100010000000.pkg`
- SHA-256: `840b0c2db7fbc2f1f6ff710c50aaf188d4916c40a151a7889ca1a0fdd986d40f`
- Playden source commit: `ae198ba5b923f242544b7dcb5a400b0f6efb30f4`
- PkgTool validation: all `[OK]`
- Parity: `hard gates passed: 0 FAIL`

This package has not been installed or published. The hosted workflow did not
upload an artifact because its earlier run failed.

## Next step

Transfer this corrected branch/build through the usual review path, then
install and hardware-test this exact Playden package before publishing
`ps4-v0.4.0`. No Orbis-Win package version or lock update is needed for this
source-path correction.

The Orbis-Win source cache under
`/home/zpeirce/codex_scratch/abi4-upstream-port-20261104/source` remains dirty
on branch `game-settings`, including local process-probe work. Do not patch or
reset it. The Orbis-Win repository checkout at `/home/zpeirce/ps4-gladius` is
clean on `launcher-settings` at tag v0.0.8. A separate targeted build of the
32-bit `kernelbase.dll` is preserved under
`/home/zpeirce/ps4-package-scratch/orbis-1012-fix/`.
