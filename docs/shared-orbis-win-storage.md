# PlayDen storage ownership

PlayDen owns its Steam installs, Wine prefixes, saves, profiles, settings, and
sign-in/session data under `/data/PLDN10001`. Half-Life 2 follows the same rule:
its Steam files are installed under
`/data/PLDN10001/games/220`; its private Wine prefix is
`/data/PLDN10001/prefixes/half-life-2`.

PlayDen does not read or write `/data/prospero-win`. Orbis-Win's installed
games, profiles, prefixes, and `dev.conf` remain independent. Imported games
are copied into Playden's own internal library; they do not share a prefix or
install record with Orbis-Win.

## External game folders

On PS4, Playden can scan game folders placed under `playden/import` on a USB
volume mounted by the console at `/mnt/usb0` through `/mnt/usb7`, then copy a
selected game into its internal library. GoldHEN is required because the normal
title sandbox cannot see `/mnt`. This access is opt-in: add `external-storage` on its own line to
`/data/PLDN10001/features.txt`:

```text
external-storage
```

Playden asks GoldHEN to remove the title's prison when the title starts, then
drops back to its original app UID so its private files remain usable. It
restores the prison before a game handoff or title exit, and re-enables access
if a handoff fails. If GoldHEN is unavailable, Playden continues sandboxed.
Removing the line restores sandbox-only behavior on the next launch.

Playden's import action copies files into Playden-owned internal storage; this
change does not add external-volume install or update actions. The PS4 formats
and mounts the USB volume (including exFAT); Playden does not provide an exFAT
driver.

PlayDen's runtime and diagnostics preferences belong in
`/data/PLDN10001/settings.conf`. The runtime settings contract uses `log`,
`log_server`, `log_port`, `wine_log`, `wait_watchdog`, `gpu_hang_report`, and
`update_check`. Steam library favorites and hidden-game choices are PlayDen-
owned and persist in `/data/PLDN10001/library-organization.txt`.

Deleting a Steam game removes its managed game files but keeps its PlayDen Wine
prefix and saves.

Older PlayDen builds may have left `/data/PLDN10001/prefixes/playden-steam-0158`
or `/data/PLDN10001/prefixes/220` behind. They are not removed automatically;
review their contents before deleting them over FTP so any wanted saves are kept.
