# PlayDen storage ownership

PlayDen owns its Steam installs, Wine prefixes, saves, profiles, settings, and
sign-in/session data under `/data/PLDN10001`. Half-Life 2 follows the same rule:
its Steam files are installed under
`/data/PLDN10001/games/220`; its private Wine prefix is
`/data/PLDN10001/prefixes/half-life-2`.

PlayDen does not read or write `/data/prospero-win`. Orbis-Win's installed
games, profiles, prefixes, and `dev.conf` remain independent. No import or
shared-prefix behavior is enabled in PlayDen.

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
