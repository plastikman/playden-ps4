# Plan: PS4 parity with Playden for PS5

## Runtime layout decision (2026-11-11)

Use the PS5 runtime's on-disk contract on PS4. Playden's game data root is
`/data/playden/games/<install_id>`; its bottle is
`library/prefixes/game`, its profile is under `library/profiles`, and a small
`/data/homebrew/FAKE78xxx/game-launch.json` names the game. The PS4 still runs
Wine inside `PLDN10001`; these folders are descriptors, not separately
registered PS4 titles. A Play request must pass a session ID to Orbis-Win, and
Orbis-Win must write the result under that game's `logs/sessions` folder.

The PS4 package generates a bundled kit view from its Wine prefix and runtime
identity. The shared `FilesystemPackageInstaller` consumes that view, and the
PS4 live composition builds a `RuntimeHost` with the shared controller. The
Orbis-Win PS4 module on branch `playden-ps4-0170` now reads descriptors
and writes session records; Playden hands a session to it with `LoadExec`. That
branch starts from the hardware-tested 01.70 code and resumes the same session
when Wine runs the next program in an execution chain.
The package build requires a clean Orbis-Win manifest and copies its module
unchanged. Hardware has not yet verified this path, so launch parity remains
open.

The new bottle is made from the matching Wine template. On first preparation,
Playden copies the old prefix's `drive_c/users`, `user.reg` and `userdef.reg`
into that bottle, flushes the files, then writes a completion marker. The old
prefix is retained for recovery. Other changes in an old `system.reg` are not
yet imported; verify a game with an existing save before considering this
migration complete.

Finish in this order:

1. Confirm from a PS4 app process that `/data/playden` and `/data/homebrew`
   can be created and written. FTP success does not prove app access. Stop
   preparation with a clear reason if either root is unavailable.
2. Port descriptor admission and session recording into the Orbis-Win PS4
   Wine module. Select `data_root/library`, the published profile and its
   `prefixes/game`; validate the descriptor, launch argument and publication
   before Wine starts. Report start, exit and failure, including a failed
   setup. The PS4 stat omission is handled through `fstat`; test the owner and
   mode checks on hardware. Validate the profile and publication against the
   descriptor before marking the runtime path ready for general use.
3. Test the wired `RuntimeHostConfig::launch_title` same-title Wine handoff.
   Check that the session record appears and that an abnormal exit reconciles
   on the next Playden start.
4. Migrate each old `/data/PLDN10001/prefixes/steam-<app>` and the special
   Half-Life 2 prefix into that game's new bottle without discarding registry
   or saves. A new template must be staged and owned before user data is
   merged. Journal and verify each move. Existing Steam game files may stay
   at `/data/PLDN10001/games/<app>`: the shared runtime records their
   `install_path` separately from `data_root`.
5. Test a prepared game, normal return, a runtime failure, an interrupted
   preparation, and an existing save on hardware. Only then make the shared
   runtime path the default player flow.

## Goal

Bring the PS4 port to product-feature parity with Playden for PS5 while keeping
the platform mechanics appropriate to PS4. Parity means that a player can
perform the same task and receives the same state, safety guarantees, and
recovery behavior. It does not require copying a PS5-only implementation such
as registering every game as a separate PS5 title.

The PS5 implementation is the reference contract:

- routes and player-facing state: `src/homebrew/simulation.{hpp,cpp}`;
- service orchestration: `src/homebrew/controller.{hpp,cpp}`;
- composition and platform wiring: `src/homebrew/ps5.cpp`;
- presentation: `src/homebrew/view.cpp`;
- persisted preferences and library organization:
  `src/app/preferences.h` and `src/library/organization.h`.

This plan records the state observed on 2026-11-09. Update the matrix whenever
a gap is completed or intentionally deferred.

## Architectural finding

PS4 presently has visual resemblance, not shared product behavior. Its five
tabs and details pages are drawn by `ps4/ui.cpp`; controller state and page
transitions live in `ps4/main.c`; account, library, details, install, Cloud,
uninstall, settings, and persistence are combined in
`ps4/platform_ps4/steam_auth.cpp`.

PS5 instead links the shared homebrew model, controller, UI, install service,
Cloud service, local store, runtime host, updater, and web service. PS4 links
selected lower-level Steam classes directly and bypasses most of that product
layer. This has already caused observable drift:

- PS4 supports one active/last download instead of the shared queue.
- PS4 Cloud handling is generic only for inspection; applying saves is still
  gated on app 220 and its hard-coded prefix.
- PS4 has basic Favorites and Hidden support, but not the complete shared
  organization model.
- PS4 installation and launch decisions use record existence and local flags
  instead of the shared details/update/runtime state.
- PS4 settings expose port bring-up controls but omit most player settings.

Do not close parity by adding each missing feature to the PS4 monolith. Move
PS4 onto the shared product model and keep only platform and runtime mechanics
behind PS4 implementations.

## Parity matrix

Implementation checkpoint, 2026-11-09: the PS4 target compiles with the
shared library cache, shared organization file, generic Cloud prefix lookup,
selected-depot and executable install checks, private prefixes for new Steam
games, and executable architecture detection. The supported PS4 download
setting was one connection until 01.86; it is now four, PS5's default. Build 01.80 was installed on the console and
aborted during startup, after cached Steam authentication and before the
library was displayed. These changes have not passed a console test.
See `ps4-parity-inventory.json` for evidence per row. The shared route and
controller model, multi-job queue, update workflows, and platform services
remain open. An installed game crashing on the older package is not evidence
that these new changes work or fail.

Local build 01.80 was produced from the clean Orbis-Win 01.62 stage, using its
released Wine module unchanged. Package SHA-256:
`8d5efeef1bb4608623615d1013477498a1d3c9ee23af042aa17244b008665cfc`.
`PkgTool.Core pkg_validate` passed all checks. It was uploaded and installed,
but its launch failed with SIGABRT. Build 01.81 fixes Mafia's trailing-slash
working-directory profile rejection and adds library-startup checkpoints.
It also aborted after the library refresh was queued. Build 01.82's worker
checkpoints showed that authentication and network fetch completed, then the
process aborted inside the library cache save. Build 01.83 skips that PS4 cache
path so the library and Mafia can be tested. It passed package validation and
was uploaded, but has not yet been launched on the console. Restoring the PS4
offline library cache remains open work. Build 01.84 exported the SDK's
extended-libc-heap settings from the UI module after Mafia's manifest inflated
size triggered a 1.6 MB `operator new` failure despite reported free memory.
That could not help: libc reads those settings only from the eboot, whose
CRT already requested an unlimited, extended heap.

Build 01.85 measured the heap with `malloc_stats_fast`: libSceLibcInternal's
heap is fixed at 13,712 KiB and never grows (flexible memory stayed at
~446 MiB while malloc refused). Playden idles near 8 MiB, so a depot manifest
(Mafia, app 40990) or the chunk buffers of more than one download worker
exhausted it. Orbis-Win found the same 13 MiB ceiling in wineserver and moved
Wine's Unix side to its own heap. Build 01.86 does the same for the UI module:
`ps4/playden_heap.c` (adapted from Orbis-Win's `pw_wine_heap.c`) serves
malloc, calloc, realloc, free and posix_memalign through `-Wl,--wrap`, backed
by anonymous (flexible) mappings, and hands pointers libc allocated back to
libc. The build fails if the module imports libc's allocator directly. On the
console 01.86 held 32 MiB at startup, downloaded Mafia past the failing
manifest, and downloaded with four connections (PS5's default, restored) at
about 10.5 MiB live with no allocation failures.
Orbis-Win's package parity script reports its expected
eboot check as failing because Playden's eboot starts `playden_ui.prx`, and its
kernelbase source check as failing on the unchanged 01.62 baseline DLL. That
script is not a Playden acceptance gate; no PS4 launch result is implied.

Status meanings:

- **Present**: the player flow and its persistence/recovery behavior exist.
- **Partial**: a useful subset exists, but it is not the PS5 contract.
- **Missing**: the shared feature is not wired into the PS4 title.
- **Platform-specific**: the result should match, but the implementation must
  differ.
- **Deferred**: deliberately planned after the current internal-storage flow.

| Area | PS5 reference behavior | PS4 state | Parity target |
| --- | --- | --- | --- |
| Navigation and UI state | Shared Home, Library, Downloads, Saves, Settings, Details, Session, Save details, and Manage games routes | Partial; five tabs and bespoke subpages | Drive all routes and focus/modal state from the shared model; render the same actions and messages |
| Home | Continue playing, recent/installed shelves, active work, notices | Partial; installed row and active download | Project the shared Home shelves, status badges, notices, and details navigation |
| Steam sign-in | QR and password, Steam Guard approval/codes, restore, stay signed in, switch account, sign out, offline/error recovery | Partial; QR and cached restore | Wire the shared account service and complete every sign-in/account action |
| Library cache | Cached/offline library, refresh, license-change refresh | Partial; `LibraryService`/`LibraryCache` are wired, but shared controller and console recovery are unverified | Use `LibraryService` and `LibraryController`, including stale/offline state and refresh |
| Library organization | Favorites, Hidden, collections, pinned collections, compatibility rating, notes, search, sort, filters, durable selection | Partial; Favorites and Hidden use shared `OrganizationSnapshot`, other actions absent | Use `OrganizationSnapshot` and its persistence controller without a PS4-only format |
| Library sources | Steam plus imported/local games | Missing local source | Use the store registry; add Local only when PS4 storage access is ready |
| Artwork | Cached covers, hero/logo/program/title art, fallbacks, failure state | Partial; cover cache | Adopt shared artwork state and cache policy; render the same fallbacks and pending/error states |
| Game details | Store metadata, install/update/play, game settings, maintenance, notes, compatibility, collections, run-once tools | Partial; metadata, install/launch/delete | Project the shared details actions and runtime/update state; remove record-exists shortcuts |
| Install choices | Storage, branch/version, content language, DLC, measured size, reservation/no-space behavior | Partial; global language and immediate install | Use the shared install sheet and size checks; keep PS4 volume reporting capability-aware |
| Downloads | Multiple jobs, pause/resume, cancel, reorder/download-now, verify/apply states, rates/ETA, completed history | Partial; one active/last job and cancel, one supported network connection | Use `InstallService` and the shared Downloads model |
| Interrupted installs | Resume journal, truthful incomplete state, repair/update of stale records | Partial; borrowed depots and launch executable checked, full record verification still pending | Validate selected depots against the record; never show Play for incomplete content |
| Game updates | Installed-game check, changed-file sizing, DLC changes, per-game and batch updates | Missing | Wire `AppDetailsService`, update-check persistence, details update, and Manage games > Updates |
| Runtime preparation | Readiness, queue, repair, recreate, settings publication, runtime update, progress and refusal reasons | Partial and PS4-specific | Add a PS4 runtime-host implementation behind a shared interface and project the same lifecycle |
| Launch/session | Preflight, Cloud-before-play, preparation, launch state, failure evidence, return and post-play work | Partial; direct same-title Wine handoff | Preserve PS4 same-title launch while exposing the shared Play/Session state machine |
| Steamworks support | Prepared globally as part of game setup | Partial; added in the PS4 launch/install path | Make it shared setup behavior for every applicable game; no app-specific branch |
| Per-game settings | Presets, executable/launch choice, graphics, architecture, overrides, input, writable folders, reset/export | Partial; generated launch profiles now use detected PE architecture and new games get private prefixes; player workflow absent | Use the shared recipes/settings contract with PS4-supported capabilities |
| Maintenance | Apply settings, verify/repair, clear shader cache, back up saves, recreate bottle, update runtime kit | Missing | Implement PS4 equivalents and gate only genuinely unsupported actions |
| Steam Cloud | Quick check, Sync all, per-game details, conflicts, use-local/use-Cloud, automatic pre/post-play sync, leftover data | Partial; per-game check/apply use a game profile prefix, shared controller and pre/post-play sync absent | Use `CloudService` and shared controller lifecycle with generic prefix/save locations |
| Uninstall | Cloud gate, runtime removal, files, keep/delete saves, queued/batch flow, truthful stopped states | Partial; deletes files and keeps prefix/saves | Use the shared uninstall flow adapted to the single PS4 title |
| Manage installed games | Updates, runtime-kit migrations, move, batch uninstall | Missing | Add shared route; mark Move unavailable until multiple writable volumes exist |
| Internal storage | Managed install root and install records | Present for current flow | Keep transactional ownership, record, and resume rules |
| External storage | Discover, select default, install/move, disconnect/reconnect | Deferred; `/mnt/ext0` is not writable from the title under current policy | Keep in the model as unavailable with a reason; implement after a reviewed PS4 access mechanism exists |
| Local import | Scan drop folders, add/play in place, copy/move into Playden, remove from library | Missing and deferred with storage access | Port the shared Local store once its PS4 directory policy and mounts are defined |
| Settings | Downloads, updates, account, storage, web access, runtime kit/about, accessibility plus game settings | Partial; language and diagnostic/runtime controls | Render shared settings, with PS4 diagnostics in an Advanced/Diagnostics section |
| Self-update | Check/download/stage/relaunch/rollback and release notes | Missing | Point to `plastikman/playden-ps4`, use PS4 package install/relaunch mechanics, and preserve the shared state model |
| Runtime-kit update | Install/admit version and update prepared games | Missing shared workflow | Define the PS4 runtime bundle contract, update it transactionally, then expose shared status/actions |
| Web access and logs | Paired local web service, redacted logs/files, in-app log viewer | Missing; only remote receiver controls exist | Port service where PS4 networking permits; always retain an in-app redacted log path |
| Download-server selection | List/test/prefer Steam CDN server | Missing | Wire `DownloadServerService`; do not expose raw concurrency as the normal player control |
| Accessibility and polish | Shared HUI layout, button glyphs, focus behavior, motion preference, font fallbacks, keyboard | Partial | Render from the shared view contract and implement required PS4 HUI/platform primitives |

## Milestone 0: correctness before parity expansion

Complete `docs/shared-depotfromapp-plan.md` first. Half-Life: Source is a
shared Playden installer defect, not a PS4-only runtime defect. The parity work
must start from an installer that cannot publish partial content as complete.

In the same milestone:

1. Validate every completed record against the current selected depot set.
2. Show `Incomplete` or `Repair` instead of `Play` when required depots or
   manifests are absent.
3. Repair old false-complete records without requiring deletion.
4. Remove app-ID checks from install, launch preparation, and Cloud paths.
   Existing checks for 220 in the PS4 adapter are migration targets, not a
   pattern to extend.
5. Keep one PS4 download connection as the supported default until the planned
   allocator/buffer-budget work passes a full four-connection hardware run.
   Hide or mark experimental the raw parallel-download selector; PS5's player
   setting is a slow-network policy, not an arbitrary thread-count control.

Acceptance games for this gate:

- Half-Life 2 (220): ordinary multi-depot install, interruption, resume, and
  launch.
- Half-Life: Source (280): `depotfromapp` content, truthful repair, and launch
  with `hl2.exe -game hl1 -steam`.
- Dreamfall (6300): generic Wine profile and global Steamworks emulator setup,
  with no title-specific installer branch.

## Milestone 1: converge on the shared product layer

### 1. Build the shared model on PS4

Add PS4 build targets for `playden_homebrew_model`,
`playden_library_controller`, `playden_persistence_controller`, and
`playden_homebrew_controller`. Resolve portability problems in shared code
rather than cloning their behavior.

Use the shared `Simulation` state as the only owner of routes, focus, modals,
library organization, jobs, save state, settings, and player-visible notices.
The PS4 main loop should translate pad/system events into model actions and
render a snapshot; it should not decide product behavior.

### 2. Replace the PS4 service monolith

Compose the same services as `src/homebrew/ps5.cpp` where platform-neutral:

- account and session repository;
- library service/cache/controller;
- persistence service/controller;
- app/store details and measurement;
- `InstallService` around the existing PS4-capable Steam installer;
- `CloudService` around the existing PS4-capable Cloud runner;
- store registry and, later, Local store;
- download-server, self-update, and web services when their platform adapters
  exist.

Split PS4-specific code into narrow adapters: native HTTP, storage roots and
directory policy, system keyboard, controller, display, logging, title
restart, and Wine runtime handoff.

### 3. Define a shared runtime-service boundary

Do not force PS5 title-registration mechanics onto PS4. Extract or introduce
the controller-facing runtime interface that reports preparation, settings,
maintenance, launch/session, save-location, and uninstall state.

- PS5 implementation: private bottle plus separately registered PS5 title.
- PS4 implementation: private prefix plus Wine relaunch inside `PLDN10001`.

Both must publish the same model states and refusal/recovery semantics.

### 4. Render the shared view contract

Preferred end state: compile `src/homebrew/view.cpp` for PS4 with a reviewed
HUI renderer/display backend. Implement missing clipping, images, text/font
fallback, focus, and modal primitives in the design system/backend rather than
forking the product view.

An acceptable intermediate step is a PS4 renderer consuming the shared
model's renderer-neutral `View`. It must not retain a second page state machine
or duplicate action eligibility in `ps4/main.c`/`ps4/ui.cpp`.

Milestone acceptance:

- A deterministic shared scenario produces the same route, rows, actions,
  enabled state, notices, and button hints on host/PS5 and PS4.
- PS4-specific code contains no library/install/Cloud business decisions.
- Existing PS5 tests and build remain unchanged in behavior.

## Milestone 2: daily-use Steam parity

Wire and validate, in this order:

1. account restoration, password/Guard flow, stay signed in, sign out, switch
   account, offline/cached library, and refresh;
2. full organization persistence: favorites, hidden, collections, ratings,
   notes, search, sort, filters, and selection;
3. install sheet: version, language, DLC, size, and internal storage;
4. multi-job Downloads queue with pause/resume/cancel/reorder, verification,
   completion history, rate, ETA, and restart recovery;
5. update detection, repair, changed-file updates, and Manage games > Updates;
6. generic uninstall with Cloud and save-retention choices.

Do not enable job concurrency beyond the PS4 hardware-supported memory budget.
Queue parity does not require concurrent downloads: multiple jobs may exist
while one network pipeline runs.

## Milestone 3: game/runtime parity

1. Generate per-game launch settings from the selected Steam launch option and
   detected executable architecture; remove the universal `pe32 + dxvk`
   assumption.
2. Give each game the prefix isolation required by its settings and repair
   lifecycle. Sharing a template is allowed; sharing mutable live prefix state
   is not.
3. Apply the global Steamworks-emulator setup transactionally to all applicable
   installations and repair older installations on next setup/launch.
4. Port Game settings, presets, program choice, input mapping, writable-folder
   mapping, detailed-log launch, and run-once tools.
5. Port repair, verify, clear shader cache, back up saves, recreate prefix, and
   runtime-kit update actions.
6. Report Starting, Running, Exited, Failed, and Crashed from evidence rather
   than treating a title restart as success.

Completion requires three unrelated Windows games to use the same generic
path without app-ID conditionals. Known game workarounds belong in data-driven
recipes, not launcher branches.

## Milestone 4: Saves parity

Replace the current PS4 Saves page's direct runner and app-220 prefix gate with
the shared `CloudService` and controller flow:

- Quick check and Sync all;
- per-game save details and byte/file counts;
- use local/use Cloud conflict handling and account confirmation;
- automatic sync before Play, after exit, and before uninstall when enabled;
- stopped/running-game safety gates;
- remove local saves and remove kept data;
- backups before destructive changes;
- uninstalled-game and disconnected-drive states.

Use runtime records to locate every game's prefix and save mappings. Never
derive Cloud capability from a hard-coded app ID.

## Milestone 5: platform services and deferred storage

After internal-storage Steam and runtime flows are stable:

1. implement PS4 self-update against `plastikman/playden-ps4`, including the
   native HTTP header-size and HTTPS-only requirements;
2. implement runtime-kit update/admission and rollback;
3. port redacted in-app logs, web access, and download-server testing;
4. investigate an authorized, title-safe external-storage access mechanism;
5. when writable volumes exist, enable default install location, move, drive
   disappearance/reappearance, local import, and play-in-place.

Until external storage is supported, shared rows should be capability-gated or
show a precise unavailable reason. Do not present a selector that can only
fail.

## Testing and evidence

### Automated parity contract

For every shared route and action added to PS4:

1. Add or extend a shared model/controller test, not only a PS4 UI test.
2. Run it normally and, where platform policy differs, with the PS4 native
   policy definition.
3. Snapshot semantic output: route, focus key, rows, actions, enabled/disabled
   state, notices, and requests. Pixel comparison is supplementary, not the
   feature contract.
4. Add platform-adapter tests for failure mapping and persistence.
5. Keep the PS5 build and full host suite passing after every milestone.

Add a small machine-readable parity inventory when implementation starts. Each
entry should name the shared action/route, PS4 support state, test name, and
hardware evidence. The document matrix remains the human roadmap; the
inventory prevents completed rows from silently regressing.

### PS4 hardware gates

Every milestone package must record:

- package version and SHA-256;
- repository commits and dirty-diff hash;
- clean install versus upgrade;
- account and storage preconditions;
- exact game/app IDs used;
- logs for start, completion, cancel/resume, failure, launch, return, and
  recovery after restart;
- whether the behavior was merely built, host-tested, or observed on PS4.

Never mark a parity row Present from a screenshot alone.

## Definition of parity complete

PS4 parity is complete when:

1. every applicable row in the matrix is Present and has automated plus PS4
   hardware evidence;
2. every deferred/platform-specific row has an explicit capability decision
   and truthful UI;
3. PS4 uses the shared product state and service orchestration rather than a
   parallel feature implementation;
4. no Steam app ID controls generic install, launch, Steamworks, prefix, input,
   or Cloud behavior;
5. incomplete or stale content cannot be labeled installed/playable;
6. interrupted operations resume or fail safely without losing the last valid
   install, settings, saves, or package;
7. the full host suite, PS5 build, PS4 build, and the milestone hardware matrix
   pass from clean, recorded inputs.

## Constraints for Luna

- Work in the current dirty tree. Do not reset, discard, or rewrite unrelated
  PS4 and downloader work.
- Implement `docs/shared-depotfromapp-plan.md` as a focused correctness change
  before starting broad parity refactoring.
- Preserve the current running console download; do not package, upload, or
  restart the title unless the owner asks.
- Prefer shared-code changes and narrow PS4 adapters. Do not add another
  app-specific exception to make a test title pass.
- Keep external storage deferred until the owner resumes it, but keep its
  parity status visible.
- Treat one download connection as supported on PS4 until the separate
  concurrent-download memory plan passes hardware acceptance.
- Split implementation into reviewable changes. Architecture convergence,
  installer correctness, account/library, queue/update, runtime, Saves, and
  platform services should not land as one diff.
