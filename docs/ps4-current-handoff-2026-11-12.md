# PS4 Playden handoff — 2026-11-12

Supersedes `ps4-current-handoff-2026-11-11.md`, whose open failures are fixed.
Ten commits on `ps4/features-0206`, `ad895ea`..`c1414d3`.

## Where the port is

Installed and verified on hardware: Playden **02.79**
(`IV0000-PLDN10001_00-PLDN100010000000-0279.pkg`). Note 02.79 is a **test
build**: it reports 2 GiB of video memory, an A/B value that proved nothing
(below). Rebuild at the default before treating it as a baseline.

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

## Open: Isaac and Hollow Knight are one Orbis-Win problem

Both are platform-level, with Titan Quest. Neither is a Playden bug, and
neither is a renderer or capability problem.

**Hollow Knight (367520)** — hangs the console ~8 s into launch, *before any
game code and before Mono loads*. It is a hang with recovery, not a crash: the
compositor stalls behind it, the system kills the title after ~30 s and the UI
is back about two minutes later. Retesting is cheap; no power cycle.

The one hard fact, from the fatal register dump:

```
mmap(addr=0x6ffffb404000, len=0x570000 (5.5 MiB), flags=0x1012 PRIVATE|FIXED|ANON)
→ 0xa0020327 SYSTEM_DEBUG_RUNTIME_SHADOW_MEMORY_ALLOC_ERROR   (×126, over 113 s)
r12 = 0x6ffffb974000 = addr + len
```

This is the **same class** as `docs/HANDOFF-2026-10-26.md`'s 1062 finding —
`0xc0000` at `0x6ffffff30000`, flags `0x1012` — already recorded there as *"the
full flexible-mapping fallback is not viable on this PS4 runtime"*. Per-page
(16 KiB) flexible mappings do work, so there is a size threshold worth finding
between 16 KiB and 768 KiB.

**The caller is a PE image mapping.** With the correct symbols:

```
__wine_syscall_dispatcher
NtMapViewOfSection          virtual.c:7368
virtual_map_section         virtual.c:4198
map_image_into_view_impl    virtual.c:3882
set_vprot                   virtual.c:2512
counted_mprotect            virtual.c:229
```

The guest maps a 5.5 MiB PE image at a fixed address; `mono-2.0-bdwgc.dll` is
almost exactly that size, which fits Hollow Knight being the only Mono game
tested.

⚠ **Symbolize against the ELF that built the shipped PRX.** There are two Wine
trees with different `ntdll.shared.elf`:
`worktrees/playden-ps4-0170/.deps/wine-ps5/prx/` (2026-11-11, **the one 0173
ships**) and `prospero-win/.deps/wine-ps5/prx/` (2026-11-04). Using the wrong
one produced a plausible but entirely fictitious backtrace
(`set_page_vprot_exec_write_protect`, `__wine_ps5_wait_for_exec_migration`,
`anon_mmap_fixed`) and sent a whole investigation down an exec-migration path
that never ran — which the absence of any `exec-migrate` record should have
flagged sooner. Both have text base 0, so offsets map directly either way and
nothing warns you.

Also note `trace_vm_api()` only fills a ring buffer a fault handler later
dumps, so a call that never returns is never recorded. That is why `api-mmap`
and `map-fault-history` show 0 records for the fatal mapping. Immediate
`sceKernelDebugOutText` is required to trace a call that can kill the process.

**The fix under test** (Orbis-Win `src/native/pw_wine_dmem_glue_ps4.c`): large
`MAP_FIXED|MAP_ANON` requests are now placed in 256 KiB pieces
(`PW_SHADOW_SAFE_CHUNK`) by `mmap_fixed_anon_chunked()`, with an immediate
`big-fixed-anon begin/end` klog line naming address, size, flags and caller.
The range migration's replacement mapping uses the same helper, since it has
the same hazard.

**Isaac (250900)** — `played_s=24` on every build. Opens a window with the
correct title and version, never paints, exits. Its only discriminator against
a working game is 80 × `exec-protect-verify` (HL2: 0) — the same
executable-memory subsystem. Needs Orbis-Win work, not settings.

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
