# Plan: PS4 parity with Playden for PS5

## Runtime layout decision (2026-11-11)

The PS4 uses the PS5 runtime's on-disk contract. A game's data root is
`/data/playden/games/<install_id>`, its bottle `library/prefixes/game`, its
profile `library/profiles`, and a small per-game title folder holds
`game-launch.json`. That title folder lives beside the host application's data:
`/data/PLDN10001` on PS4, `/data/homebrew` on PS5 (`src/runtime/title_root.h`).
The PS4 still runs Wine inside `PLDN10001`; these folders are descriptors, not
separately registered PS4 titles. Play passes a session ID to Orbis-Win, which
writes the result under that game's `logs/sessions`.

The PS4 package describes its bundled Orbis-Win as a kit (`scripts/ps4-kit-view.py`)
and the shared `FilesystemPackageInstaller` consumes that view;
`ps4/runtime_ps4/` supplies the kit, a native coordinator whose titles are
profile names, and the legacy-prefix migration. The runtime bundle is named
`wine-wow64`, which the profile validation compares against the profile's own
`runtime` field.

**Observed on hardware, 2026-11-11 (PlayDen 02.67, Orbis-Win 01.73):** Portal 2
(620) prepared, published, launched through `LoadExec`, reached its main menu,
quit, and Playden read the session back:
`play finished install=steam-620 state=5 code= cleared=1 played_s=275`.
Half-Life: Source (280) and Dreamfall (6300) also launched. Three defects had to
be fixed first, each measured rather than inferred:

1. Orbis-Win's descriptor check hard-coded `/data/homebrew/`; it now validates
   against the title's own data root (`prospero-win` 1f53082).
2. Orbis-Win's diagnostics chmodded the game's `library` folder to 0777 on every
   launch, which the evidence reader then refused; under Playden the log folders
   stay owner-only (`pw_diagnostics_private_modes`, `prospero-win` 4a89b39).
3. A PS4 title cannot `dup()` a directory descriptor (EPERM), so every
   `dup`+`fdopendir` listing read as empty and the directory walk took each child
   for a swapped entry (ESTALE). `platform::OpenHeldListing` lists by path on the
   console. Both `getdents` calls work on either kind of descriptor; only `dup`
   does not.

Open on this path:

- A game prepared with an earlier kit refuses both Play and "Update games to the
  new runtime kit" (`installed title and bottle are not this install's from a
  compatible kit`); only Maintenance > Recreate bottle recovers it, and each
  Recreate leaves a `<install>.set-aside-<uuid>` bottle copy.
- Legacy prefix migration (`ps4/runtime_ps4/legacy_bottle.cpp`) copies an old
  `/data/PLDN10001/prefixes/steam-<app>` into the new bottle; a game with an
  existing save has not been verified end to end.
- Cloud sync before and after play, "Run launcher or setup", and detailed-log
  launches are wired but unexercised.

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

**Resolved 2026-11-11, with one step left.** The PS4 title now links and runs
the shared product layer: `ps4/homebrew_live.cpp` composes the same services the
desktop host's live mode composes (account, library, organization, details,
installs, Cloud, download servers, store details, community recipes, local
store) and hands them to the shared `Controller` and `RuntimeHost`, drawn by
`src/homebrew/view.cpp` through a CPU backend of hui's renderer (`ps4/hui_cpu/`).
Playden's own UI is what the title draws by default since 02.68; the first PS4
launcher runs only when the switch file says `legacy`. Retiring `ps4/ui.cpp`,
`ps4/main.c`'s page state and most of `ps4/platform_ps4/steam_auth.cpp` is the
remaining step. Two services are not wired yet: web access and self-update.

The finding that led there, recorded 2026-11-09:

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

**Checkpoint 2026-11-11 (PlayDen 02.68, Orbis-Win 01.73).** The PS4 runs the
shared product layer, so most rows below are the shared implementation rather
than a PS4 one. "Shared" means the shared code is wired and compiled into the
title; it does **not** mean the row has the hardware evidence this plan requires
(see *PS4 hardware gates*). What one session observed on the console is noted per
row and, in detail, under *Runtime layout decision*.

Still genuinely absent: web access and self-update are not constructed on PS4;
external storage stays deferred. The first PS4 launcher still exists behind the
`legacy` switch file and has not been removed.

The 2026-11-09 checkpoint that preceded this work (library cache, heap ceiling,
download connections, Mafia's manifest) is kept in git history; its conclusions
about the PS4 monolith are superseded by *Architectural finding*.

Status meanings:

- **Present**: the player flow and its persistence/recovery behavior exist.
- **Partial**: a useful subset exists, but it is not the PS5 contract.
- **Missing**: the shared feature is not wired into the PS4 title.
- **Platform-specific**: the result should match, but the implementation must
  differ.
- **Deferred**: deliberately planned after the current internal-storage flow.

| Area | PS5 reference behavior | PS4 state | Parity target |
| --- | --- | --- | --- |
| Navigation and UI state | Shared Home, Library, Downloads, Saves, Settings, Details, Session, Save details, and Manage games routes | Shared, since 02.68 by default; the shared model drives every route | Drive all routes and focus/modal state from the shared model; render the same actions and messages |
| Home | Continue playing, recent/installed shelves, active work, notices | Shared; Continue playing, shelves, notices observed | Project the shared Home shelves, status badges, notices, and details navigation |
| Steam sign-in | QR and password, Steam Guard approval/codes, restore, stay signed in, switch account, sign out, offline/error recovery | Shared account service; cached sign-in carried over, other actions untested | Wire the shared account service and complete every sign-in/account action |
| Library cache | Cached/offline library, refresh, license-change refresh | Shared `LibraryService`/`LibraryCache`; 118-game library observed | Use `LibraryService` and `LibraryController`, including stale/offline state and refresh |
| Library organization | Favorites, Hidden, collections, pinned collections, compatibility rating, notes, search, sort, filters, durable selection | Shared `OrganizationSnapshot` and persistence controller | Use `OrganizationSnapshot` and its persistence controller without a PS4-only format |
| Library sources | Steam plus imported/local games | Shared registry; Local store wired, untested | Use the store registry; add Local only when PS4 storage access is ready |
| Artwork | Cached covers, hero/logo/program/title art, fallbacks, failure state | Shared artwork service and cache; no banner where Steam has none (upstream gap) | Adopt shared artwork state and cache policy; render the same fallbacks and pending/error states |
| Game details | Store metadata, install/update/play, game settings, maintenance, notes, compatibility, collections, run-once tools | Shared details actions, runtime and update state | Project the shared details actions and runtime/update state; remove record-exists shortcuts |
| Install choices | Storage, branch/version, content language, DLC, measured size, reservation/no-space behavior | Shared install sheet; PS4 reports no free space | Use the shared install sheet and size checks; keep PS4 volume reporting capability-aware |
| Downloads | Multiple jobs, pause/resume, cancel, reorder/download-now, verify/apply states, rates/ETA, completed history | Shared `InstallService` and Downloads model | Use `InstallService` and the shared Downloads model |
| Interrupted installs | Resume journal, truthful incomplete state, repair/update of stale records | Shared; an unfinished download is adopted as a paused job | Validate selected depots against the record; never show Play for incomplete content |
| Game updates | Installed-game check, changed-file sizing, DLC changes, per-game and batch updates | Shared `AppDetailsService` and update-check persistence; kit-update dialog observed | Wire `AppDetailsService`, update-check persistence, details update, and Manage games > Updates |
| Runtime preparation | Readiness, queue, repair, recreate, settings publication, runtime update, progress and refusal reasons | Shared `RuntimeHost` with a PS4 kit and coordinator; prepare observed | Add a PS4 runtime-host implementation behind a shared interface and project the same lifecycle |
| Launch/session | Preflight, Cloud-before-play, preparation, launch state, failure evidence, return and post-play work | Shared Play/Session state machine; launch, exit and play time observed | Preserve PS4 same-title launch while exposing the shared Play/Session state machine |
| Steamworks support | Prepared globally as part of game setup | Shared setup through `RuntimeHost`, no app-specific branch | Make it shared setup behavior for every applicable game; no app-specific branch |
| Per-game settings | Presets, executable/launch choice, graphics, architecture, overrides, input, writable folders, reset/export | Shared recipes/settings contract; private settings storage works | Use the shared recipes/settings contract with PS4-supported capabilities |
| Maintenance | Apply settings, verify/repair, clear shader cache, back up saves, recreate bottle, update runtime kit | Shared menu; Recreate bottle observed, other actions untested | Implement PS4 equivalents and gate only genuinely unsupported actions |
| Steam Cloud | Quick check, Sync all, per-game details, conflicts, use-local/use-Cloud, automatic pre/post-play sync, leftover data | Shared `CloudService` and controller wired; sync untested | Use `CloudService` and shared controller lifecycle with generic prefix/save locations |
| Uninstall | Cloud gate, runtime removal, files, keep/delete saves, queued/batch flow, truthful stopped states | Shared uninstall flow wired; untested | Use the shared uninstall flow adapted to the single PS4 title |
| Manage installed games | Updates, runtime-kit migrations, move, batch uninstall | Shared route wired; Move unavailable (one volume) | Add shared route; mark Move unavailable until multiple writable volumes exist |
| Internal storage | Managed install root and install records | Present for current flow | Keep transactional ownership, record, and resume rules |
| External storage | Discover, select default, install/move, disconnect/reconnect | Deferred; `/mnt/ext0` is not writable from the title under current policy | Keep in the model as unavailable with a reason; implement after a reviewed PS4 access mechanism exists |
| Local import | Scan drop folders, add/play in place, copy/move into Playden, remove from library | Shared Local store wired; untested, and `drop_folder_access.cpp` still lists through `dup` | Port the shared Local store once its PS4 directory policy and mounts are defined |
| Settings | Downloads, updates, account, storage, web access, runtime kit/about, accessibility plus game settings | Shared settings render | Render shared settings, with PS4 diagnostics in an Advanced/Diagnostics section |
| Self-update | Check/download/stage/relaunch/rollback and release notes | Missing from the shared composition; the first PS4 launcher's own updater remains | Point to `plastikman/playden-ps4`, use PS4 package install/relaunch mechanics, and preserve the shared state model |
| Runtime-kit update | Install/admit version and update prepared games | Shared dialog present; refuses games prepared with an earlier kit (Recreate bottle recovers) | Define the PS4 runtime bundle contract, update it transactionally, then expose shared status/actions |
| Web access and logs | Paired local web service, redacted logs/files, in-app log viewer | Missing; `web::Service` is not constructed on PS4 | Port service where PS4 networking permits; always retain an in-app redacted log path |
| Download-server selection | List/test/prefer Steam CDN server | Shared `DownloadServerService` wired; untested | Wire `DownloadServerService`; do not expose raw concurrency as the normal player control |
| Accessibility and polish | Shared HUI layout, button glyphs, focus behavior, motion preference, font fallbacks, keyboard | Shared view contract on the CPU renderer; ~58 fps, frames skipped when idle | Render from the shared view contract and implement required PS4 HUI/platform primitives |

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

**Done 2026-11-11, except removing the old launcher.** Steps 1-4 below describe
how it was approached; what shipped differs in one way worth recording: instead
of porting the shared model piece by piece onto the PS4 UI, the PS4 adopted the
whole app layer at once (`ps4/homebrew_live.cpp` composing the desktop host's
live services, `src/homebrew/view.cpp` drawn by `ps4/hui_cpu/`), which made most
of the per-feature porting unnecessary. Web access and self-update are the two
services still unwired.


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

## What is left (2026-11-11)

1. Remove the first PS4 launcher: `ps4/ui.cpp`, the page state in `ps4/main.c`
   and most of `ps4/platform_ps4/steam_auth.cpp`, once the shared path has run
   without the `legacy` escape for a while.
2. Wire web access and self-update into the PS4 composition.
3. Recover games prepared with an earlier kit without Recreate bottle, and stop
   `.set-aside-*` bottles accumulating.
4. Exercise on hardware, with the evidence the gates below require: Cloud sync
   before and after play, uninstall, Manage installed games, game updates, the
   Local store (and replace `drop_folder_access.cpp`'s `dup` listings first),
   download-server selection, and a legacy-prefix migration for a game with an
   existing save.
5. Keep deferred rows truthful in the UI: external storage, Move.

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
