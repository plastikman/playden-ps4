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
