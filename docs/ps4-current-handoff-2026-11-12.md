# PS4 Playden handoff — 2026-11-12

Supersedes `ps4-current-handoff-2026-11-11.md`, whose open failures are fixed.
Ten commits on `ps4/features-0206`, `ad895ea`..`c1414d3`.

## Where the port is

Latest build: Playden **02.81** (`…-0281.pkg`) on **Orbis-Win 01.75**
(`pkgout/wine-app-relocatable`, manifest `app_ver=01.75`), carrying the
per-page executable-range migration. 02.79 was a throwaway A/B that reported
2 GiB of video memory and proved nothing; do not use it as a baseline.

What now works that did not yesterday:

- **The Local store**, end to end: scan, load, add, move to storage, uninstall.
- **"Update games to the new runtime kit"** for Source-engine games. Left 4
  Dead 2, Portal, Portal 2 and Hollow Knight update in place; no Recreate.
- **The kit stops changing on every build**, so the update prompt no longer
  appears at every launch.
- **The UI says PS4**, not PS5, in 146 strings.
- Half-Life 2, Half-Life: Source and Left 4 Dead 2 launch and play.

## The pattern worth remembering

**PS5 derives kit capabilities inside `kit_manifest.cpp`'s verifier. PS4 never
runs that verifier** — `AdmitPs4Kit` builds the `VerifiedKitView` from
`playden-kit-view.json`. So anything PS5 *derives* is simply absent on PS4
unless `scripts/ps4-kit-view.py` states it, and the simulation hardcodes
several of them true, so the fixtures UI can never show the gap.

Three capabilities were shipped-but-unclaimed before today. **Audit the rest of
`game_settings.h`'s `kKit*` list against what Orbis-Win actually stages** rather
than finding them one game at a time:

| capability | provider that was already in the package | symptom |
|---|---|---|
| `audio_ps5` (`template.audio_ps5`) | `sce_module/wineps5.prx` | every `audio=ps5` recipe refused: "lacks what this game needs (PS5 audio)" — the kit update could never succeed for HL2/HL:Source/L4D2/Portal |
| `opengl-zink` | `win/mesa-zink/{i386,x86_64}-windows/opengl32.dll` | OpenGL games fell back to a PS5 OpenGL SDK path PS4 lacks |
| `settings-profile-environment` | `pw_ps4_setenv`, `radv.env` reader | **still unclaimed**; the whole `[environment]` section is unavailable on PS4 |

## Facts worth not rediscovering

- **klog is the guest diagnostic channel on PS4.** `PS4_DEV_CONF` only carries
  the Playden UI PRX: 0 `PW_` records in 296k lines of
  `/data/PLDN10001-network.log`. The Wine guest writes `wine-ps4:` and
  `ntdll ps4-pageflt-*` records to klog (port 3232). It is **not** true that
  guest logs are unreachable; a dead receiver made it look that way.
  `PS4_DEV_CONF` also writes a *file*, `/data/PLDN10001-network.log`, not only
  a TCP stream — look there before concluding diagnostics are missing.
- **Check the coredump watcher is tailing the *live* klog.** A watcher left
  from a previous day tails a file nothing writes any more, and captures
  nothing. This lost the first Hollow Knight crash.
- **The bottle template was non-reproducible**, and that is why every game was
  offered a kit update after every install. `OwnerMarker()` writes the kit's
  `files_sha256` into each game's `.playden-title-owner`/`.playden-stage-owner`,
  and `TitleCurrent()` is just "does the marker still match". `wineboot --init`
  rewrote 68,348 registry timestamps, four per-boot GUIDs, and
  `HardwareInformation.MemorySize` **from the build machine, sampling live
  memory** (two builds minutes apart wrote 8.77 GB and 8.98 GB).
  `scripts/canonicalize-prefix-registry.py` pins all of it; three consecutive
  builds then produced an identical kit. One earlier build still differed for a
  reason never identified — worth a few more builds before calling it airtight.
- `HardwareInformation.MemorySize` is under the **display adapter** class
  (`{4D36E968-...}`): it is **video** memory, not system RAM. The template also
  still advertises the build host's `llvmpipe` as the display adapter.
- **`SITE CHMOD` is the way through any "550" under `/data`.** GoldHEN's FTP
  runs as a user with no write permission on an owner-only tree, so `DELE`,
  `RMD` and `RNFR` all fail there regardless of filename. `HELP`/`SITE HELP`
  are not implemented, so the command is undiscoverable. A **sticky** parent
  (`/data/playden/games` is `drwxrwxrwt`) is a second gate: chmod it to 777,
  remove, then restore 1777. Never widen a live bottle, and never widen a
  game's `logs` directory — Orbis-Win's own comment records that widening it
  made Playden refuse the session and **the PS4 cannot chmod it back**.
- The PS4's GPU is **gfx7 BONAIRE/Liverpool**, so `RADV_PERFTEST=pswave32` and
  the `pixel_shader_waves` setting are no-ops here. Leo's "wave64 freezes the
  console" note is PS5/RDNA-specific.
- Playden **detects the renderer from the game's files** ("Chosen from the
  game's files: OpenGL"); the recipe can override it. It is not guesswork and
  it got Isaac right.

## Hollow Knight: the shadow-memory crash is fixed (Orbis-Win `7df46bd`)

**Two facts from Zak that made this tractable**, and that bound the whole
problem: Hollow Knight is the **first 64-bit game** to run on Orbis-Win, and it
**works on PS5**. So this is PS4-only, and 32-bit games never reached it
because WoW64 maps their images low and in smaller pieces.

The chain, fully established:

```
NtMapViewOfSection -> virtual_map_section -> map_image_into_view_impl
  -> set_vprot -> mprotect_range -> counted_mprotect -> __wine_ps5_mprotect
  -> migrate a 5.5 MiB executable range, direct memory -> flexible, in ONE request
  -> 0xa0020327 SYSTEM_DEBUG_RUNTIME_SHADOW_MEMORY_ALLOC_ERROR
```

**The shadow pool is exhausted by cumulative bytes, not by request size.** A
first attempt split the request into 256 KiB pieces; it did not fix anything
but it measured the limit exactly — seven pieces (1.75 MiB) landed and the
eighth failed, with `rsi` showing the chunk size, `rdi` the cursor and
`r13`/`r12` the original range. That ruled chunking out, and the chunking was
reverted.

The fix: above `PW_PS4_EXEC_MIGRATE_RANGE_MAX` (256 KiB) a range falls to the
existing per-page path, which the surrounding code already calls "the proven
per-page behavior". Measured on hardware with 02.81 / Orbis-Win 01.75:

| | before | after |
|---|---|---|
| `SHADOW_MEMORY_ALLOC_ERROR` | 126 | **0** |
| compositor timeouts | 22 | **0** |
| survives | ~8 s then a 113 s hang | **minutes** |
| `exec-range-per-page` | — | 17 ranges, `0x44000`–`0x90000`, all `prot=0x5` |

**Still open: Hollow Knight now hangs later.** It reaches thread creation
(`supplied stack=…`) and then emits **zero** guest records, with no
`play finished`. A different stopping point, not this one. A fresh run with a
healthy receiver should capture the whole startup.

**Isaac (250900)** — Zak found `--luaheapsize=0` stops it dying; it then runs
without crashing but the screen stays black. Note Isaac never produced a single
`SHADOW_MEMORY_ALLOC_ERROR`, so that was always a different failure from Hollow
Knight's, even though both are large-allocation stories. With the crash gone,
Isaac is now a **rendering** problem, which is where `opengl-zink` finally gets
a chance to matter. Worth checking whether Zink's `opengl32.dll` actually
reached its bottle (the capability only exists from 02.78, so it may need a
repair).

## Tooling: do not trust the klog receiver

It **silently stalled three times** on 2026-11-12, each time with an
established socket to `:3232`, so `ss` showed it healthy while it captured
nothing. It cost the first Hollow Knight crash and a whole startup trace. Check
that the **file has grown** in the last N seconds, not that the socket exists,
and re-check it immediately before any launch worth capturing.

Two related traps, both cost time today:

- `trace_vm_api()` fills a ring buffer a fault handler dumps later, so a call
  that kills the process is **never traced**. Use `sceKernelDebugOutText`
  directly to trace anything that can be fatal.
- A guest record's timestamp can be the time of a **buffered flush**, not when
  it happened. 205 records sharing one timestamp is a flush, not a burst.

## Dead ends — do not re-walk these

- **`result=declined` write retries** — HL2 has 63 of them and runs fine.
- **`ps4-pageflt-after handled=0` at `0x7bf2e493`** — 12 in Hollow Knight,
  **12 in HL2, same address**.
- **Thread/stack exhaustion** — HL2 creates 24 thread stacks to Hollow
  Knight's 5 and never raises the error.
- **Reported video memory** — 8 GiB vs 2 GiB changed nothing: 128 vs 126
  shadow errors, 5 vs 5 stacks. `PS4_GUEST_MEMORY_BYTES` now takes an override
  if it ever needs retesting.
- **`WINE_PS5_EXEC_REFUSED`** — set on every launch as a *path*; HL2 sets it
  too. Not a symptom.
- **Zink for Isaac** — the capability was genuinely missing and worth fixing,
  but it did not change Isaac at all.
- **`pixel_shader_waves` / wave64** — no-op on gfx7 (see above).

## Still open on the Playden side

1. `PointBottleAtKitMono` fails for **every** game: `cannot read the bottle's
   user.reg: mode=438`. `ReadRegularAt` accepts only 0600 or 0644 for a private
   file and the bottle's `user.reg` is 0666, so the bottle is never pointed at
   the kit's Wine Mono. Harmless for Source games; unexamined for Mono games.
2. **Settings → Quit Playden does not quit** (Confirm had no effect twice).
   Low priority — the PS button is what players use.
3. `refresh-ownership` still needs a Recreate for a game whose setup predates
   the current kit; Recreate works and is ~80 s.
4. Web access and self-update are still not constructed in the PS4
   composition, and `LogsPaths` is configured with PS5-shaped roots
   (`homebrew`, `kits`) that do not exist on PS4.
5. Remove the first PS4 launcher once the shared path has run a while.

## Steam network play port in progress

The next upstream unit is `8bc63d0` (Play Online games with Valve's Steam
client), which follows `938026d`. Its shared Playden changes are in the working
tree: Steam network settings and online gating, friends, the Steam client
installer/host, online-session handoff, and the shared UI. The PS4 composition
in `ps4/homebrew_live.cpp` now supplies its account session, HTTPS downloads,
friends session, and persona preference. Console-facing strings use
`PLAYDEN_CONSOLE_NAME` where the feature text is shared.

This is not ready to package or install. The upstream Orbis-Win `leo/main`
already completed the PS5 implementations: `3f07187` (title notifications and
invites, Nov 9), `3c436a8` (a helper process, Nov 11), and `99a20ad` (helper
messages and invite questions, Nov 11). The PS4 Orbis-Win branch
`sync-trial-2` does not contain those commits; its current checkout is at
`75f2535` with other local changes. The remaining work is to adapt those
upstream features to PS4 and advertise `helper_process` and `title_notify` in
the PS4 kit view. The settings stay capability-gated until then. No build or
tests have been run for this port. The upstream `docs/online-play.md` describes
the PS5 implementation and still needs reconciliation with PS4 behavior before
the port is done.

The prerequisite `37e7489` is also in progress: the native parent/child probe
now has a PS4 OpenOrbis build and packaging path under `probes/local-process/`.
Its Wine shared-server variant remains PS5-only reference material pending a
stable PS4 Orbis-Win interface. The PS4 probe has not been built or run.

`33b541b` (Manage DLC on an installed game) is now ported in the working tree:
Maintenance offers the DLC sheet, applying it queues a DLC-choice update, and
the choice survives an interrupted update. Turning DLC off can complete with
zero downloaded bytes, which is still reported as an update. This change has
not been built or tested.

## Build and test

```
# Playden (default build -- omit PS4_GUEST_MEMORY_BYTES)
ORBIS_WIN_ROOT=~/ps4-gladius ORBIS_WIN_RELEASE=~/ps4-gladius/build/playden-orbis-0173 \
DXVK_262_DIR=~/codex_scratch/quickstart-test/ps4-wine/dxvk-2.6.2/x32 \
PS4_DEV_CONF=10.168.2.188:9300 PKG_VERSION=02.NN JOBS=6 scripts/build-ps4-runtime.sh
```

Always run `scripts/check-ps4-package-parity.py <baseline stage> <candidate
stage> --elf-dir <prx dir>` and require exit 0 before installing. Evidence for
today's runs, including klog slices, parity logs, per-build symbols and the
Hollow Knight crash report, is under
`/home/zpeirce/codex_scratch/playden-test-20261111-postreboot/`.

`8c25557` (Group the Steam settings and trim Also called) has now been ported
in the working tree: Steam options are grouped under their own Compatibility
heading, and the matching row-count logic includes that group. Existing PS4
console-name macros remain in the invite text. This port is unbuilt and
untested.

`58985c9` (Give online play its own Game settings section) is now ported in
the working tree: Steam features, sign-in, and invites appear under an open
Online heading when `online-steam` is enabled. With it disabled, those rows
fall back under Compatibility › Steam. Invite text continues to use
`PLAYDEN_CONSOLE_NAME`. This port is unbuilt and untested; `e6a8df0` remains.

`e6a8df0` (Share Online in recipes, keep the overlay out) is now ported in
the working tree. Community recipes can carry Steam features = Online, while
Steam sign-in, invites, and personal settings (including the performance
overlay) are excluded from shared settings and share-change summaries. Online
defaults are ignored when `online-steam` is disabled. This port is unbuilt and
untested.

`16a06c6` (Check the title's runtime for every gated capability) is now
ported in the working tree. `RuntimeHost::TitleKit` asks
`KitDependsOnTitleRuntime` instead of maintaining a partial capability list,
so `helper_process` and future gated capabilities trigger the title-runtime
check. I also repaired the malformed capability-name initializer left by the
online-play port. This remains unbuilt and untested.

`160fc80` (Close Game settings while a game waits for the PS5) is now ported
with console-neutral wording: the row stays disabled until Finish setup, and
trying to open settings explains that changes would miss the game's first
start. This port is unbuilt and untested.

`3467544` (Open Game settings on a summary of changes) is now ported in the
working tree. Game settings opens on the preset and the player's changed rows;
“Change settings…” opens the full editor, and collapsed sections summarize
changes within them. PS4 online-setting fallback grouping is retained. This
port is unbuilt and untested.

`08839b6` (Add Touchpad hold to swap to a mouse preset) is ported behind the
`touchpad-switch` capability: Playden can write and validate the `[launcher]
chain = off` profile, and older title runtimes defer it until their kit is
updated. The current PS4 kit does not advertise this capability, so the setting
remains unavailable until Orbis-Win implements and reports the PS4 behavior.
The upstream PS5 dependency pin was not applied. This port is unbuilt and
untested.

`0ec4ece` (Correct BioShock's controller note) is ported: the compatibility
entry now documents the in-game controller setting and the mouse/launcher ways
to reach it. This documentation change was not tested on PS4.

`7ec9542` (Remove the Project Zomboid file watcher stub) is partially ported:
the recipe format can declare retired shipped files, removes only unchanged
copies by SHA-256 during preparation, and serializes/rejects the field in the
same cases as other shipped-only recipe data. The PZ stub and its recipe entry
remain in place, and the dependency pin was not advanced: upstream's required
Wine patch 1019 is explicitly PS5-only and is not present in this PS4 runtime
tree. Removing the stub before equivalent PS4 many-directory watch support is
available would reintroduce PZ's startup failure. This port is unbuilt and
untested; finish the removal once PS4 runtime support is ported and accepted.

`810a83d` (Turn community recipes on for everyone) is ported: community recipes,
sharing, votes, test reports and account linking no longer depend on the
`community-recipes` feature line. `online-steam` remains opt-in. The docs use
the PS4 data-folder path, and the existing test now expects community recipes
to be enabled in release defaults. This change is unbuilt and untested.

`91cdb30` (Use Kit 0.24) was audited but its PS5 manifest hashes, wrapper
revision and runtime artifact revision were not copied into the PS4 lockfile.
Kit 0.24 includes Wine patch 1019, which the Orbis-Win content audit marks
absent, plus PS5 package artifacts. Advance the PS4 kit metadata only after its
matching Orbis-Win changes are present and the candidate package passes parity.

`ed10194` (Start the Unity player, not its bootstrap) is ported: generated
recipes now detect a Unity bootstrap beside its player and start the player
directly; recipe refresh moves older generated entries too. Explicitly chosen
programs remain as chosen. Local imports use the same Unity/Unreal launcher
selection. This change is unbuilt and untested.

`a86c633` (Start Borderlands 2, not the 2K launcher) is ported: XNA detection
now requires the Game or Graphics assembly/namespace (or
`GraphicsDeviceManager`), so the 2K launcher is treated as a launcher. Older
generated recipes are rebuilt with the game's executable first while preserving
Steam launch entry ids and folders; a program explicitly selected by the player
keeps its settings and is marked with the new defaults revision. This change is
unbuilt and untested.

`621c30f` (Make a game's Cloud save folders before it starts) is ported: the
Steam app-info mapping records validated folders below the Wine user's profile
in the per-game Steamworks facts. During preparation, the package layer creates
missing folders in an existing bottle, matches existing names case-insensitively,
and logs additions or failures without refusing launch. This change is unbuilt
and untested.

`1be7fd3` (Let a later install script's value replace an earlier one) is
ported: install-script registry values now replace earlier values with the
same case-insensitive hive key and value name, both during language selection
within a script and when merging scripts in filename order. The Far Cry 3 note
no longer mentions the duplicate-value preparation failure. This change is
unbuilt and untested.

`245d64e` (Move prospero-win to the steamapps title shell) is partially
ported: Playden now pins prospero-win at `2ddfbbf`, and the shared helper
`pw_steam_view.c` is included in the relevant CMake contract and publication
targets. The PS4 title shell integration and `steam-library-path` kit
capability are still pending; no kit metadata was changed. This source and
build-wiring update is unbuilt and untested.

`c602643` (Let each Steam game choose its library folder) is ported in the
settings and simulation layers: new Steam installs get Steam's folder as a
Playden-owned per-game default, while previously installed games stay on their
existing Z: paths. The Compatibility › Steam row is gated on
`steam-library-path`, applies after the game closes, and can be overridden by
the player. Reset, preset/recipe changes, and sharing keep the install default
separate from player choices. Profile publication writes `[steam]` only when
Steam's folder is selected. The PS4 kit still lacks the capability, so the row
is unavailable there for now. The client-library registration step is now
ported in `f8815de`; it remains dormant until the PS4 runtime advertises the
needed capabilities. No tests or builds were run for this change.

`f8815de` (Tell Steam's client where a Steam game is installed) is ported: the
runtime can list each Online game's bottle as a Steam library and write that
game's app manifest from its install record. The work is gated on Online mode,
the helper process, an installed Steam client, and the selected Steam folder;
the PS4 runtime currently supplies neither `helper-process` nor
`steam-library-path`, so the step cannot run yet. The runtime writer uses
managed bottle paths and refuses symlinked paths in the shared client folder.
No tests or builds were run.

`1e3c477` (Show Steam games under `steamapps\\common`) is ported in Playden:
Steam app details provide the validated `installdir`, preparations persist it
on the game's install record, and profile publication writes `[steam]` only
when the verified kit advertises `steam-library-path` and the game's title
uses that kit's runtime. Existing preparations fall back to the app ID; other
stores keep their real paths. Refused-launcher detection maps names under the
Steam common folder back to the game install. The PS4 kit does not advertise
this capability yet, so the behavior stays dormant until the title shell is
ported. No tests or builds were run for this change.

`c4b8ac2` advances the `prospero-win` source pin to `df71ecc`, which makes
virtual-link lookup case-blind in Wine (`ntdll.prx` and `wineserver.prx`). The
PS4 runtime artifacts and kit metadata remain unchanged: this fix reaches PS4
only after the matching Orbis-Win/runtime changes are ported and a kit is
built. This dependency pin is unbuilt and untested.

`050ce11` improves Steam Cloud failure reporting: failed session opens now
retain the CM stage, transport category, and relevant result code; Cloud checks
and transfers can show whether DNS failed, Steam asked Playden to wait, Steam
timed out, or its response could not be read. Cloud logs record the failing
step without logging endpoints or addresses. Caller-canceled operations are
marked canceled and are not saved as persistent Cloud failures. The player
message uses `PLAYDEN_CONSOLE_NAME`, so this PS4 build says PS4. No tests or
builds were run for this change.

`353382b` updates Project Zomboid's compatibility entry: game version 42.22
rejects the recipe's unsigned file-watcher shim because the `zombie` package
is signed. The entry now marks it broken and records that the required
many-directory watcher support is not in the PS4 runtime. Its Kit column is
left unset because this PS4 build has not been verified against that game
version. The compatibility document date is updated to 2026-11-12. No tests
or builds were run.

`ab20633` updates the inherited PS5 compatibility list from 12 community
reports: adds five working games and four broken games, refreshes the results
for The Forest, Silver Pines and Murky Divers, and adds two community-recipe
reports. These PS5 reports do not establish PS4 compatibility.
No tests or builds were run.

`080ea53` updates the inherited PS5 compatibility list from 12 reports: adds
working reports for 3dSen and imported Moadra, updates Euro Truck Simulator 2
to working on kit 0.23 and Dying Light to kit 0.24, marks Clair Obscur partial,
adds five other partial reports and three broken reports, including the
imported Need for Speed III entry. These PS5 kit results don't establish PS4
compatibility. No tests or builds were run.

`5c62811` updates the inherited PS5 compatibility list with player-report
counts: Portal 2 now has three reports, and Shadow of the Tomb Raider has one.
These counts reflect PS5 reports and don't establish PS4 compatibility. No
tests or builds were run.

`03df916` updates the inherited PS5 compatibility list from player reports:
adds I Am Bread as working on PS5 kit 0.23, increments Little Nightmares
Enhanced Edition's report count to two, and removes Far Cry 3's stale duplicate
registry-value preparation note (fixed by `1be7fd3`). These are PS5 reports;
they do not establish PS4 compatibility. No tests or builds were run.

`54b4d4e` updates `homebrew_installed_locations_test` to wait for the
asynchronous startup Cloud check of Steam game 220 before asserting that the
check covered it. The test keeps its existing 60-second deadline and only
waits for the Cloud result in the scenario that verifies Cloud behavior. The
test was updated but not run.

`e46a715` further hardens `homebrew_installed_locations_test`: the imported
game scenario waits until the Play row is enabled before pressing Cross, and
timeout diagnostics identify which wait expired and report the open modal and
notice counts. The test was updated but not run.

`e6c7f84` removes the divider between the game-settings summary and “Change
settings…”. Other dividers remain. The upstream test assertions for this layout
aren't present in the PS4 simulation test file, so I changed only the UI action
list. No tests or builds were run.

`ca42bcb` removes the Windows user-folders byte count from the games-list
payload and web page, including its sort key. The PS4 page has four remaining
columns (the upstream page has an extra Store column), so the empty-row span is
four here. API fixture data no longer supplies the removed field. No tests or
builds were run.

## Orbis-Win v0.0.6 artifact / Playden 02.83 build

The successful Orbis-Win CI artifact is run `43354514410` (`work-20261112`,
root `0e11c00`, `prospero-win` `3cca6a0`). Its Orbis-Win package is
`IV0000-GLDS10043_00-GLDS100430000000.pkg`, app version 02.03, SHA-256
`5d606d16bf5be85bcc351f334e849bc6a24a0ec15783dc39322e7f310baa77a3`. The CI
package validation and Orbis-Win parity report pass.

Built Playden **02.83** from `ps4/upstream-ports` at `e37433e`, using that
artifact, the matching host Wine and DXVK 2.6.2. Installer:
`IV0000-PLDN10001_00-PLDN100010000000.pkg`, SHA-256
`2e3dc11a5ec8d851d1f8578a3a44364c5a99f3579a177f884d2dffc16e1fbf13`. The
build added three missing PS4 CMake source entries across commits `847537c`,
`ace7aa7`, and `e37433e` (Steam friends, online-session/client/library helpers,
and the `pw_steam_view` helper).

The package parity command against the preserved Playden 02.82 stage exited 0.
It reported two PS4 baseline strings absent from this runtime:
`big-fixed-anon` and `exec-range-per-page`. The latter is the known per-page
executable-range fix from Orbis-Win `7df46bd`; the artifact's build provenance
says that fix was reverted because Fallout: New Vegas faulted at `0x1f88`.
The local check also could not match runtime PRX build symbols because the CI
artifact contains the package but not its unstripped ELF intermediates. The
CI parity report independently passes its exact PRX export and signal-mask
checks; it does not account for the dropped 02.82 behavior.

The generated kit view advertises only `input-keyboard-stick`,
`input-touchpad`, `opengl-zink`, `settings-dxvk-config`, and `title-any-id`.
It does **not** advertise `helper-process`, `title-notify`, or
`steam-library-path`; Steam network features therefore remain unavailable
through the kit gate. The Playden package contains the Steam host payload, but
that does not supply the missing Orbis-Win capabilities.

At Zak's direction, the validated 02.83 installer was uploaded to the PS4 at
`/data/pkg/IV0000-PLDN10001_00-PLDN100010000000.pkg` for the broad integration
test. GoldHEN FTP reports the expected size, 1,001,717,760 bytes, and the local
PKG validates. The package has not been installed yet. The available
`ps5mcp install` path requires Web File Manager on port 8888, which is not
running on this PS4; the installer is staged for the console's HDD package
source. This test knowingly includes the missing `exec-range-per-page` fix and
does not enable Steam network play because the required capabilities are
absent. No gameplay launch or tests were run.
