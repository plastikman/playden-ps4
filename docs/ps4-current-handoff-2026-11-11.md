# PS4 Playden handoff — 2026-11-11

Supersedes the morning version of this file, whose open failures are all fixed.

## Where the port is

The PS4 title runs **Playden's own app layer, services and runtime**, not a
parallel implementation. Released as
[`ps4-v0.1.0`](https://github.com/plastikman/playden-ps4/releases/tag/ps4-v0.1.0):
package 02.70 (`04bf5f87…`) on Orbis-Win 01.73, from `a5b4edb` with a clean tree.

Observed on hardware, with no switch file present (the path a fresh install
takes): the shared UI comes up live with the player's account, library and
games; Portal 2 prepares, launches, reaches its main menu, quits, and Playden
reads the session back — `play finished install=steam-620 state=5 code=
cleared=1 played_s=275`. Half-Life: Source and Dreamfall also launch.

## What changed today

1. **Adopted the shared app layer** (`5f8a63b`). `ps4/homebrew_live.cpp`
   composes the desktop host's live services (account, library, organization,
   details, installs, Cloud, download servers, store details, community
   recipes, local store) and hands them to the shared `Controller` and
   `RuntimeHost`; `src/homebrew/view.cpp` draws through a CPU backend of hui's
   renderer (`ps4/hui_cpu/`). `ps4/runtime_ps4/` supplies the PS4 kit (the
   bundled Orbis-Win, described by `scripts/ps4-kit-view.py`), a native
   coordinator whose titles are profile names, and the legacy-prefix migration.
2. **Made Play work end to end** (`deb18d7`, Orbis-Win `1f53082`, `4a89b39`).
3. **Made Playden's UI the default** (`7bde4ba`); the switch file is now an
   opt-out (`legacy`, `fixtures`, `WIDTHxHEIGHT THREADS`).
4. **Released `ps4-v0.1.0`** (`4c7cb65`, `a5b4edb`) and taught the updater to
   find it.
5. **Upstream PR [#827](https://github.com/leolobato/playden-ps5/pull/827)**
   for the borrowed depots, which Half-Life: Source running finally justified.

## Facts worth not rediscovering

- **A PS4 title cannot `dup()` a directory descriptor** (EPERM, measured). Both
  `getdents` calls work on either kind of descriptor; only `dup` is refused. So
  `dup`+`fdopendir` listings read as empty, and the directory walk then took
  every child for a swapped entry and refused it with `ESTALE` — which broke the
  mode repair, the self-heal and the session-evidence read at once. Use
  `platform::OpenHeldListing(fd, path)`. The Local store scanner and drop-folder
  access/removal walks now use this helper on PS4. Their native-policy tests
  simulate directory `dup()` failing with `EPERM`.
- The SDK's libc directory functions are **link stubs**, and `getdents` and
  `sceKernelGetdents` both live in libkernel. Only the console can answer
  questions about them; `ps4/homebrew_live.cpp`'s probes measure them.
- **Orbis-Win must not widen Playden's folders.** Its diagnostics chmodded the
  game's `library` to 0777 every launch (correct for its own FTP-read logs,
  fatal under Playden); `pw_diagnostics_private_modes` keeps them owner-only.
  The cost is that Wine's per-game logs are no longer FTP-readable — use the
  network log (`PS4_DEV_CONF=<host>[:<port>]` stages `/app0/dev.conf`, read
  before the `/data` grant; a release build omits it).
- **Every Orbis-Win rebuild changes the kit identity.** Games prepared with the
  previous kit then refuse both Play and "Update games to the new runtime kit";
  only Maintenance > Recreate bottle recovers them, ~80 s each, and each leaves
  a `<install>.set-aside-<uuid>` bottle copy.
- **ABI-4 pins**: `build-app-ps4.sh` still defaults to ABI-3 and its ABI-4
  defaults are stale. Pass `WOW64NATIVE_ABI=4` with the PE from a released stage
  and `…/prx/sce_module/wow64native.prx`
  (`e531a51b…` / `20eee436…`), as `scripts/ci/build-abi4-package.sh` does.
- **Tags**: the fork carries upstream's `v0.x.0` tags, so the port tags
  `ps4-v<version>`. The updater parses an optional name plus `-` before the
  version (Orbis-Win `fe5c8d1`).
- A file the PS4 build must not modify in `third_party/prospero-win` goes in
  `ps4/vendor/orbis-win/` instead (see its README).

## Build environment

Work moved to `linux-install` (10.168.2.188); the old host is `slackian`
(10.168.2.47, reachable by ssh name, still holds the full Orbis-Win deps).

```
# Playden
ORBIS_WIN_ROOT=~/ps4-gladius ORBIS_WIN_RELEASE=~/ps4-gladius/build/playden-orbis-0173 \
DXVK_262_DIR=~/codex_scratch/quickstart-test/ps4-wine/dxvk-2.6.2/x32 \
PKG_VERSION=02.NN JOBS=6 scripts/build-ps4-runtime.sh
# add PS4_DEV_CONF=<host>:9300 for a test build that logs to a PC

# Orbis-Win's Wine PRX (then copy it into a stage and update that manifest)
WOW64NATIVE_ABI=4 WOW64NATIVE_DLL=<stage>/win/wine/lib/wine/x86_64-windows/wow64native.dll \
WOW64NATIVE_ABI4_SHA256=e531a51b… WOW64NATIVE_ABI4_PRX_SHA256=20eee436… \
PW_ROOT=~/ps4-gladius/worktrees/playden-ps4-0170 TITLE_ID=GLDS10043 \
PW_BUILD_ID=GLDS10043-01.NN-… bash scripts/build-relocatable-wine-ps4.sh
```

The build folder is a symlink to `~/junk/playden-build` (the 125 GB disk).
PkgTool needs Debian 11's `libssl1.1`; OpenSSL 3 is refused.

The console is driven through the `ps4` MCP (Chiaki Remote Play), registered by
the committed `ps4-gladius/.mcp.json`. **Hold every key ~300 ms** — short taps
are dropped, and ~400 ms on PS opens the Quick Menu instead of Home. Snapshot
after each step; do not assume a press landed.

## What is left

### Latest hardware run — 2026-11-11 post-reboot

The console recovered after reboot. The earlier crash window is retained at
`/home/zpeirce/codex_scratch/playden-test-20261111-188/klog.log`; it contains no
guest exception context and no captured dump, so the cause remains unknown.

The Local store test exposed and fixed three separate issues:

1. Playden 02.71 fixed `drop_folder_access.cpp` directory listings, but the
   scanner still used `dup()` and could not scan on PS4.
2. Playden 02.72 changed the scanner to `platform::OpenHeldListing`. Native and
   simulated-PS4 scanner tests passed. Hardware showed `import scan candidates=0`
   because the PS4 composition mounted the Local store at
   `/data/PLDN10001/playden-import`, while the folder is `/data/playden-import`.
3. Playden 02.73 corrected the Local store mount to `/data`. On hardware the
   Import games screen found `PlaydenSmoke` at
   `/data/playden-import/PlaydenSmoke` and listed its nested `Smoke.exe`.
   Selecting it then failed with “Playden's list of imported games could not be
   read.” The matching `install.log` says `imports load failed` at startup and
   `imports are not loaded` on Add. Scanning works; adding is still blocked by
   the import store load and needs investigation. The smoke folder is test data,
   not a real game.

4. Playden 02.74 fixed the load. Every `Storage` under the title's data root now
   calls `AllowTraversableRoot()`: the root the system gives a PS4 title is 0711,
   and `Storage`'s durable journal path wants the 0700 the private policy
   demands, so the read failed before it reached the file. Verified on hardware
   2026-11-12: startup no longer logs `imports load failed`; Import games lists
   `PlaydenSmoke` with its path (not "could not be read"); Add succeeds
   (`import added id=bb43f986-d1c9-48e7-8d89-0f83030fedd9
   folder=playden-import/PlaydenSmoke program=Nested/Smoke.exe`); and "Move to
   storage" succeeds (`import move renamed from=/data/playden-import/PlaydenSmoke
   to=/data/PLDN10001/games/local-bb43f986-...`, `import move complete files=1`),
   with the drop folder left empty and the detail page showing Location: PS4
   storage. Uninstall then completed the cycle (`import copy uninstalled
   id=bb43f986-...`): `games/local-bb43f986-...` is gone and `.playden-games`
   shrank from 286 to 228 bytes. Scan, load, add, move and uninstall all pass on
   hardware; the Local store gate is done.

   **`PS4_DEV_CONF` logging writes a file, it does not only stream over TCP.**
   Nothing ever connected to `ps5logd` on 9300, but the same lines are in
   `/data/PLDN10001-network.log` (39 MB on 2026-11-12), which FTP can read.
   Look there first; do not conclude the diagnostics are missing because the
   listener stayed silent. The probe in that file measures the cause exactly:

       durable root private:     open=13   fsync=-1/0     <- EACCES
       durable root traversable: open=ok   fsync=0/0

The installed test build is Playden 02.74,
`IV0000-PLDN10001_00-PLDN100010000000-0274.pkg`, SHA-256
`421477ec16d6ba1f00d119ed60dba1ec0e43ddcab1432568c9c5c2601a5a01f7` (02.73 was
`d3bc23f9...`). Its
package parity gate passed against 02.70. Package hashes, matching UI symbols,
klog, import logs and the earlier 02.70–02.73 build records are under
`/home/zpeirce/codex_scratch/playden-test-20261111-postreboot/`; see
`PROVENANCE.md` there. The Local store fixes are uncommitted in
`cmake/PlaydenCore.cmake`, `ps4/homebrew_live.cpp`,
`src/stores/local/drop_folder_access.cpp`, `src/stores/local/scanner.cpp`, and
their two tests. On Linux the full host preset cannot configure because the
direct UI build currently supports macOS only; the targeted native-policy
scanner and drop-folder tests passed.

In the order I would take it:

1. **Remove the first PS4 launcher** — `ps4/ui.cpp`, the page state in
   `ps4/main.c`, most of `ps4/platform_ps4/steam_auth.cpp` — once the shared
   path has run a while without the `legacy` escape. Keeping two UIs alive
   costs double for everything.
2. **Wire web access and self-update** into the PS4 composition; they are the
   only shared services not constructed.
3. **Recover from a kit change without Recreate bottle**, and stop
   `.set-aside-*` bottles accumulating.
4. **Finish the Local store test** — find why the import store cannot load on
   PS4, then verify Add/remove and copy/move behavior with a controlled folder.
   After that, exercise the other parity gates: Cloud sync before and after play,
   uninstall, Manage installed games, game updates, and a legacy-prefix
   migration for a game with an existing save.
5. **Offer the PS4 seams upstream** where they are general: the time-text patch,
   and the artwork banner fallback (Steam has no `library_hero.jpg` for older
   games such as Half-Life: Source, and only the cover has a fallback today).

Not ours: Isaac (250900) faults in the guest with the Steam emulator, and
Portal 2 shows a black screen for ~40 s while loading.

## Leftovers on the console

Both leftovers resisted removal on 2026-11-12, and neither is reachable the
obvious way:

- The five `steam-6300.set-aside-*` bottles cannot even be **listed** over FTP.
  `pw_diagnostics_private_modes` made `/data/playden/games` owner-only, so FTP
  returns 550 and the "~1.6 GB" figure here is unverified. They would have to go
  through Playden's own Maintenance screen.
- The stray `/data/data/PLDN10001/FAKE78xxx` folders (4 of them, ~1.1 MB of
  duplicate icons, no `game-launch.json` -- the real ones at
  `/data/PLDN10001/FAKE78xxx` have it and are intact) cannot be deleted by FTP:
  every `DELE` and `RMD` under `/data/data` returns 550, because the FTP server
  has no write permission on that tree, unlike 0777 `/data/pkg`. PS4-Xplorer
  2.0 in Root Access mode shows them and offers Delete, but the operation hangs
  with an empty progress bar and its Cancel stops responding too; the app exits
  on its own and nothing is partially deleted. A GoldHEN payload or shell is
  probably the only route left. Low stakes either way. `/data/playden-ui-preview` was deleted to test the new
default; it said `1280x720 6 live`.
