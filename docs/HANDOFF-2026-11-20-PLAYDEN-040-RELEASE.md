# Playden PS4 v0.4.0 release handoff — 2026-11-20

## Current state

The release work is on branch `codex/ps4-v0.4.0-release`, currently at
`1534e351`. It is rebased on current `origin/main` and pushed. The branch sets
`ps4/VERSION` to `0.4.0`, package APP_VER to `02.99`, and pins Prosper-Win to
published commit `eaccec6d3382fa4ab61e35d8cc72908aa67ac164`.

Playden v0.4.0 targets Orbis-Win v0.0.8, package 02.98. Because that release
does not publish a build manifest, the Playden workflow now uses
`scripts/ci/orbis-win-v0.0.8.lock` to bind the source tag, root commit,
installer filename, SHA-256, app version, and runtime build marker. The
runtime kit records v0.0.8 as its artifact revision and labels unpublished
source-level provenance as unavailable. Local package extraction verified the
locked digest and build marker.

The package workflow now runs `PkgTool.Core pkg_validate` and
`check-ps4-package-parity.py` before artifact upload. The validator needs both
`DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1` and the cached OpenSSL 1.1 directory
on `LD_LIBRARY_PATH`; both are now set in `scripts/ci/build-package.sh`.

## Build result and release gate

The Playden 02.99 installer assembled at about 1,021 MB and passed `PkgTool`
validation. The parity gate passed the relocation chain, PS4 RADV markers,
Orbis-compat split, baseline payload presence, driver-thread providers, and
current dmem glue. It reported one hard failure:

> kernelbase no_debugger path never calls log_exception_context32:
> PW_EXCEPTION32 is compiled in but unreachable in a title (port archive 1012)

The gate ended with `NOT INSTALLABLE: 1 FAIL`, so the workflow did not upload
an artifact or publish a release. This is a property of the v0.0.8 Orbis-Win
payload, not a Playden build failure. The 32-bit `kernelbase.dll` contains 25
`PW_EXCEPTION32` strings, but its ordinary no-debugger exception path cannot
reach the logger. Matching `ntdll.shared.elf` and `wineserver.shared.elf` were
also unavailable, so the parity run skipped those export/signal-mask checks.

Run `44756310013` contains the full report. A local copy of the failed log is
at `/home/zpeirce/ps4-package-scratch/playden040-failed-3.log`. No v0.4.0
package was installed or released.

## Next step

To satisfy the required install gate, rebuild the 32-bit `kernelbase.dll` with
the PS4 exception-path fix from archive patch 1012, place it in a new Orbis-Win
package, and rerun parity. Then update Playden's locked Orbis-Win package/tag,
build APP_VER 02.99 again, and require both package checks to pass before
uploading or publishing `ps4-v0.4.0`.

The Orbis-Win source cache under
`/home/zpeirce/codex_scratch/abi4-upstream-port-20261104/source` is dirty on
branch `game-settings`, including local process-probe work. Do not patch or
reset it. The Orbis-Win repository checkout at `/home/zpeirce/ps4-gladius` is
clean on `launcher-settings` at tag v0.0.8. Establish a separate, revertible
source worktree before attempting the 1012 fix, and rebuild only the affected
32-bit module if the pinned build inputs permit it.
