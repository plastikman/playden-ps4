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
  `platform::OpenHeldListing(fd, path)`. `drop_folder_access.cpp` still has
  three raw `dup` listings.
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

In the order I would take it:

1. **Remove the first PS4 launcher** — `ps4/ui.cpp`, the page state in
   `ps4/main.c`, most of `ps4/platform_ps4/steam_auth.cpp` — once the shared
   path has run a while without the `legacy` escape. Keeping two UIs alive
   costs double for everything.
2. **Wire web access and self-update** into the PS4 composition; they are the
   only shared services not constructed.
3. **Recover from a kit change without Recreate bottle**, and stop
   `.set-aside-*` bottles accumulating.
4. **Exercise with real evidence** (the gates in the parity plan): Cloud sync
   before and after play, uninstall, Manage installed games, game updates, the
   Local store — replace `drop_folder_access.cpp`'s `dup` listings first — and a
   legacy-prefix migration for a game with an existing save.
5. **Offer the PS4 seams upstream** where they are general: the time-text patch,
   and the artwork banner fallback (Steam has no `library_hero.jpg` for older
   games such as Half-Life: Source, and only the cover has a fallback today).

Not ours: Isaac (250900) faults in the guest with the Steam emulator, and
Portal 2 shows a black screen for ~40 s while loading.

## Leftovers on the console

Five `steam-6300.set-aside-*` bottles (~1.6 GB) and the stray
`/data/data/PLDN10001/FAKE78xxx` folders from the doubled-path bug, both
awaiting a decision. `/data/playden-ui-preview` was deleted to test the new
default; it said `1280x720 6 live`.
