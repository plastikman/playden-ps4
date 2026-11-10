# Plan: resolve shared Steam depots from another app

## Finding

Half-Life: Source (app 280) is currently published as installed after PlayDen
downloads only depot 280. Its appinfo also lists Source engine depots from app
220 with `depotfromapp`, but those entries do not carry their own manifests.
The hardware log shows PlayDen rejecting depots 221, 222, and the related
language depots with reason `branch`, selecting only depot 280, and writing a
725,859,105-byte completed install record. The generated launch profile is
valid (`hl2.exe -game hl1 -steam`); the installed payload is incomplete.

This is shared PlayDen behavior, not a PS4 runtime issue. Both PS5 and PS4
compile `src/steam/backend/steam_install_runner.cpp` and the depot selection in
`src/steam/backend/app_info.cpp`.

Commit `c3bdd8e` added source-app lookup only for demos through
`KeepDepotsDemoBorrows`. Its test explicitly says non-demo borrowers remain
unchanged. General games that use `depotfromapp` therefore still lose required
content when the borrowing app's listing has no manifest.

## Required behavior

For every depot listed by the requested app with `depotfromapp=<source app>`:

1. Fetch the source app's appinfo along with the requested app's DLC appinfo.
2. From that source app, retain only depots that the requested app explicitly
   borrows. Never pull the source app's unrelated depots or DLC.
3. Feed those retained listings into `SelectDepots`. Its existing duplicate
   resolution should replace the borrowing app's manifest-less entry with the
   source app's manifest while preserving ownership as content of the requested
   game.
4. Preserve `source_app=<source app>` and
   `depot_from_app=<source app>` so depot keys and manifest request codes are
   requested under the correct app.
5. Do not require ownership of the source app. The existing `depotfromapp`
   license rule already permits a borrowed depot when the selected game's
   package grants it.
6. Treat source appinfo as required metadata. If it cannot be fetched or parsed,
   stop the install with a resumable error. Never continue with only the
   borrowing app's own depots and never write a completed install record.

## Implementation map

### 1. Generalize borrowed-depot extraction

Files:

- `src/steam/backend/app_info.h`
- `src/steam/backend/app_info.cpp`

Replace the demo-specific extraction with a general helper, for example:

```cpp
void KeepDepotsBorrowedFrom(const AppInfo& borrower, AppInfo* source);
```

The helper should keep a source depot only when `borrower.depots` contains the
same depot ID with `depot_from_app == source->app_id`. Set the retained depot's
`depot_from_app` to the source app ID and clear the source app's DLC list.
Keep `KeepDepotsDemoBorrows` as a wrapper if that avoids unnecessary call-site
churn, or replace it after updating all tests.

Do not restrict the match to demos, and do not copy every depot from the source
app. Let `SelectDepots` detect conflicting manifests as it does today.

### 2. Fetch required source apps during installation

File: `src/steam/backend/steam_install_runner.cpp`

Build a unique set of source app IDs from every nonzero `depot_from_app` in the
requested app. Add those IDs to the existing batched appinfo fetch beside DLC
apps and a demo's parent.

Track required source apps separately from optional DLC appinfo:

- A DLC appinfo fetch may still be omitted under the existing rules.
- A borrowed source appinfo fetch or parse failure must stop both a fresh
  install and an update.

When a fetched app is both DLC and a borrowed source, keep its ordinary DLC
listing for DLC selection and add a second trimmed copy for borrowed depots.
This prevents unrelated source-app content from entering the base install.

Use a specific log step such as `source_app_info` with the source app IDs and
the transport or parse result. The player-facing failure should say that Steam
could not provide required shared game content and that Resume will retry.

### 3. Use the same related appinfo for details and updates

Files:

- `src/steam/backend/cm_app_details.h`
- `src/steam/backend/cm_app_details.cpp`

The current `FetchDlcApps` path has the same demo-only logic. Extend it to fetch
and trim borrowed source apps too. A rename to `FetchRelatedApps` is reasonable
if kept within this change.

This is needed so:

- install size includes borrowed depots;
- branch and language choices use the same depots as installation;
- `CompareWithRecord` sees missing borrowed depots in an older record as an
  available repair/update.

### 4. Repair old false-complete records

The existing app 280 record contains only depot 280. Once related appinfo is
resolved, common PS5 details should compare the new selection with that record
and report an update because depots 221 and 222 are absent.

The native PS4 shell currently decides installed state from the presence of the
record and launches directly. Wire its installed-game action to the common
update path, or add a shared selection validation before launch so a record
missing required borrowed depots is shown as `INCOMPLETE` / `REPAIR` rather
than playable. Keep this generic; do not special-case app 280.

For the first hardware proof, deleting and reinstalling app 280 is acceptable,
but the implementation is not complete until an existing false-complete record
can be repaired without deleting the whole game.

## Tests

### Pure depot selection

Extend `tests/steam_app_info_test.cpp` with a normal game modeled on app 280:

- app 280 lists depot 280 with a manifest;
- it lists depots 221 and 222 with `depotfromapp=220` and no manifests;
- app 220 supplies manifests for 221 and 222 plus an unrelated depot.

Verify:

- before source resolution only 280 is selected and 221/222 are rejected as
  `branch`;
- after source resolution 221, 222, and 280 are selected in the borrowing
  app's listing order;
- 221/222 have `source_app=220`, `owner_app=280`, and
  `depot_from_app=220`;
- the unrelated app 220 depot is absent;
- app 220 does not need to be in `owned_apps`;
- conflicting manifests still fail selection;
- the existing demo behavior remains unchanged.

Replace the test assertion that explicitly leaves non-demo borrowers unchanged.

### Installer behavior

Add a runner test with fake appinfo responses proving that it requests app 220
after reading app 280, passes all three selected depots to the installer, and
writes all three to the completed record.

Add failure cases for source appinfo transport failure, missing response, and
unparseable response. Each must leave no completed install record and return a
retryable result only for network/timeout errors.

### Details and repair

Extend the app-details tests with an installed record containing only depot 280.
After supplying the trimmed app 220 listing, details must report an update whose
target includes 221 and 222. Verify install and download sizes include the
borrowed depots.

## Validation

1. Run the targeted appinfo, app-details, and installer tests through CTest.
2. Run the full host test suite required by `AGENTS.md`.
3. Build PS5 and PS4 targets to prove the shared change compiles on both.
4. On PS4, install a package containing the fix, remove the current app 280
   install for the initial clean test, and reinstall Half-Life: Source.
5. Confirm the log selects depots 221, 222, and 280 and the install record names
   all of them.
6. Confirm the installed tree contains the shared Source engine files, then
   launch `hl2.exe -game hl1 -steam` through the existing profile.
7. Recreate or retain an old depot-280-only record and confirm the UI offers a
   repair/update instead of Play.

## Constraints for Luna

- Work in the current dirty tree without resetting or discarding the PS4 port
  and downloader changes.
- Keep the fix in shared Steam code; avoid PS4-only or app-280 branches.
- Do not treat a source appinfo failure as optional.
- Do not publish a completed record until all selected borrowed depots are
  installed and the existing transactional commit succeeds.
- Do not build or push a console package while the current Half-Life 2 download
  is running unless the owner asks to interrupt it.
