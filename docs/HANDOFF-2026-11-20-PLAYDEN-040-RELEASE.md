# Playden PS4 v0.4.0 release handoff — 2026-11-20

## Current state

The release work is on branch `codex/ps4-v0.4.0-release`, at
`c0d001b633658a6cdb4ac9f3d4beba5ef4a27f0f`, pushed to `origin`. It is based
on current `origin/main`. The branch sets `ps4/VERSION` to `0.4.0`, package
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

## Playden 02.99 release result

The hosted workflow `44763828090` completed successfully on
`c0d001b633658a6cdb4ac9f3d4beba5ef4a27f0f`. It built and published the
public, non-prerelease GitHub release [`ps4-v0.4.0`](https://github.com/plastikman/playden-ps4/releases/tag/ps4-v0.4.0)
with Orbis-Win v0.0.8 (02.98). The release includes the installer and build
manifest. The manifest records package SHA-256
`50033a5d0a6661f652b7326e5ee6787bc989ab2c30bee4a0cc60357651279fa1` and
Orbis-Win package SHA-256
`6932b8532766a33afca2a386697a62b8a6c95cc2c46802fc38f185c8edb8d3bf`.

A local rebuild from this branch passed PkgTool validation and parity with
**0 hard failures**. The parity report still warns that matching
`ntdll.shared.elf` and `wineserver.shared.elf` are unavailable, so those
export/signal-mask checks are skipped. The hosted build used the locked v0.0.8
Orbis-Win package.

Artifacts are under
`/home/zpeirce/ps4-package-scratch/playden040-gatefix/`:

- Installer: `IV0000-PLDN10001_00-PLDN100010000000.pkg`
- Local rebuild SHA-256: `840b0c2db7fbc2f1f6ff710c50aaf188d4916c40a151a7889ca1a0fdd986d40f`
- Hosted release SHA-256: `50033a5d0a6661f652b7326e5ee6787bc989ab2c30bee4a0cc60357651279fa1`
- Playden source commit: `c0d001b633658a6cdb4ac9f3d4beba5ef4a27f0f`
- PkgTool validation: all `[OK]`
- Parity: `hard gates passed: 0 FAIL`

The installer filename is `IV0000-PLDN10001_00-PLDN100010000000.pkg`.

## Follow-up

The release is published, but a later size audit found that the hosted package
omitted the optional Wine Mono and vkd3d-proton kit payloads. The workflow did
not pass `WINE_MONO_DIR` or `VKD3D_PROTON_DIR`, and the builder allowed both to
be empty. See
[`HANDOFF-2026-11-20-PLAYDEN-041-RELEASE.md`](HANDOFF-2026-11-20-PLAYDEN-041-RELEASE.md)
for the corrected 0.4.1 build. Do not use 0.4.0 as proof of complete runtime-kit
payload parity.

The v0.4.0 hardware installation and game testing remain unreported. No
Orbis-Win package version or lock update was needed for the source-path
correction.

The Orbis-Win source cache under
`/home/zpeirce/codex_scratch/abi4-upstream-port-20261104/source` remains dirty
on branch `game-settings`, including local process-probe work. Do not patch or
reset it. The Orbis-Win repository checkout at `/home/zpeirce/ps4-gladius` is
clean on `launcher-settings` at tag v0.0.8. A separate targeted build of the
32-bit `kernelbase.dll` is preserved under
`/home/zpeirce/ps4-package-scratch/orbis-1012-fix/`.
