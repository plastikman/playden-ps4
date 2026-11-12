# PS4 PlayDen handoff — 2026-11-11

## Current state

- Worktree: `/home/zpeirce/git/playden-ps4`; PS4 port is still broadly uncommitted. Do not reset or discard unrelated edits.
- Console: `10.168.2.52`, GoldHEN FTP port `2121`.
- Package upload destination: `/data/pkg/IV0000-PLDN10001_00-PLDN100010000000.pkg`.
- Latest package built and uploaded: **02.57**, SHA-256 `e0483b5406472a24e15dad238b313784de08d067252fc4329ba7f736d148019b`; PkgTool validation passed. User is installing/testing it.
- Build inputs: `ORBIS_WIN_ROOT=/home/zpeirce/ps4-gladius`, `ORBIS_WIN_RELEASE=/home/zpeirce/ps4-gladius/build/playden-orbis-0171`, `DXVK_262_DIR=/home/zpeirce/codex_scratch/quickstart-test/ps4-wine/dxvk-2.6.2/x32`. Build invocation is in prior conversation; latest build log `build/ps4-runtime/build-0257.log`.

## Confirmed PS4 paths

- Game installs: `/data/PLDN10001/games/<appid>`.
- Per-game bottle: `/data/playden/games/steam-<appid>/library/prefixes/game` (the shared runtime’s bottle layout).
- Profiles: `/data/playden/games/steam-<appid>/library/profiles`.
- PS4 managed title folders: `/data/PLDN10001/<FAKE78xxx>`.
- `/data/homebrew` is PS5-specific and must not be used by the PS4 implementation. `src/runtime/title_root.h` selects `/data/PLDN10001` under `PLAYDEN_PLATFORM_PS4`.

## Active failures and evidence

1. **Bottle setup / Finish setup fails reading Wine registry.** 02.55 diagnostic log found `user.reg` mode decimal 448 = octal `0700`; reader only permits `0600` or `0644`. 02.56 changed template-copy mode for registry hives, but existing bottles are retained and therefore not recopied. 02.57 added a rewrite-to-0600 repair in `FilesystemPackageInstaller::SettleBottleModes` for existing hives. Confirm it actually runs and fixes the latest console failure. Latest available log `build/ps4-runtime/runtime-live-latest.log` (22:28:53Z) still has app 550 failing with `mode=448`; confirm whether that log predates installation/use of 02.57. Dreamfall app 6300 was deleted by user; they are downloading it again. Saves are not a priority.
2. **Half-Life: Source (app 280) title staging collision.** 22:28:04Z log: `from=stage-title ... code=title-stage-collision message=staged native title is not owned by this install`; repeat at 22:28:10Z. App reserved `FAKE78001`. This is a title-stage ownership/recovery problem, separate from the bottle registry failure. Investigate why `/data/PLDN10001/FAKE78001` or its staging state survives without the expected ownership marker. Never redirect this to `/data/homebrew`.
3. The old runtime kit rejects HL2 recipe 220 for missing PS5 audio capability; likely recipe metadata/platform capability mismatch. Do not treat as current bottle issue.

## Useful logs / next actions

- Fetch runtime log: `python3 /home/zpeirce/ps4-gladius/scripts/ftp-get.py 10.168.2.52 2121 /data/PLDN10001/logs/runtime.log build/ps4-runtime/runtime-live-latest.log`
- Inspect latest app_version in `/home/zpeirce/codex_scratch/playden-test-20261111/klog-2.log`.
- First determine whether 02.57 is installed and whether its existing-bottle normalization runs. Check `SettleBottleModes` ordering and ensure the rewrite happens before `PointBottleAtKitMono` / registry publication reads `user.reg`.
- Then fix stale native title staging safely: only remove/recreate a stage when ownership can be established; never recursively delete an arbitrary path. Preserve install records and package user data.
- User prefers autonomous progress, brief status updates, no speculative paths, and no repeated confirmation prompts. They said they are switching to Claude to finish the MCP work, so leave a clear status and avoid broad unrelated cleanup.
