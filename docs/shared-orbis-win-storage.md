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

## External USB storage

The PS4 mounts supported FAT/exFAT USB media at `/mnt/usb0` through
`/mnt/usb7`; Playden does not include a filesystem driver. GoldHEN is required
because the normal title sandbox cannot see `/mnt`. External access is opt-in:
add `external-storage` on its own line to `/data/PLDN10001/features.txt`:

```text
external-storage
```

Playden asks GoldHEN to remove the title's prison when the title starts, then
drops back to its original app UID so its private files remain usable. It
restores the prison before a game handoff or title exit, and re-enables access
if a handoff fails. If GoldHEN is unavailable, Playden continues sandboxed.
Removing the line restores sandbox-only behavior on the next launch.

When enabled, attached writable USB volumes are offered as game install
destinations under `playden/games`. Playden can also scan `playden/import` and
copy a selected game into its internal library. Steam installs and moves use
Playden's normal install journal. The external write path is implemented but
still needs a PS4 test with FAT and exFAT media; until that passes, treat
external installs and moves as unqualified. Keep the drive attached through an
operation and use the PS4 safe-removal action before unplugging it. Reconnect
the drive before launching games installed there.

The PS4 may report a mounted FAT/exFAT volume with the same `st_dev` as `/mnt`.
Playden therefore identifies a USB mount using its world-writable mode and uid
`-1` as a fallback; an empty USB slot remains root-owned and read-only. The
import log records mode, uid and device for each mount so missing and
unwritable volumes are distinguishable.

External game files live on FAT/exFAT, which does not preserve Unix ownership or
permissions. The admitted runtime kit must declare `game-root-unowned` and
`game-root-usb-search` for Playden to publish and launch such installs. Prefixes,
profiles, saves, and session state remain in Playden's internal data root. USB
availability is refreshed when Playden starts; restart it after connecting a
drive or safely removing it.

Console acceptance should verify the flag-off behavior, import scan, Steam
install to USB, launching a game from USB with the required kit capabilities,
moving it back to internal storage, reconnecting it, and recovery after an
out-of-space failure. Test writes on a disposable FAT and exFAT volume first.

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
