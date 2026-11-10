# Playden for PS4: native title and local runtime

## Product flow

Playden is one native PS4 title. It signs in to Steam, shows the library,
downloads game files to the console, and creates a private Wine environment
for each game. Selecting Play starts the Windows game locally inside the same
title using the PS4 Wine, Mesa, DXVK, input and audio work from orbis-win.
When the game exits, the title restarts into Playden's library view.

There is no external `GLDS10043` installation in this design. Users do not
create or copy orbis-win profiles. Internally, Wine still needs executable,
arguments, prefix, graphics and input settings. Adapt Playden's existing
transactional per-game publication to the PS4 runtime's parser; keep those
files private to Playden and generated from game settings.

## Milestone 1: native PS4 shell

Status on 2026-11-08: `PLDN10001` boots on PS4, renders the native Hui
library shell, and responds to DualShock 4 navigation across all five tabs.
The owner confirmed the selector styling and reported that the latest input
response feels much less laggy. QR sign-in is hardware-confirmed on the native
`libSceHttp`/`libSceSsl` transport. This iteration connects Playden's existing
`LibraryClient` to the PS4 shell: after authentication it requests Steam's
owned games on a worker thread, then displays the returned game names. The
owner confirmed that 12 games load on hardware. The launcher separates filter
focus from game-list focus and opens a selected-game detail screen. On hardware,
Half-Life 2 returned its Windows launch option and 12 Windows depots. The PS4
curl build must retain WebSocket support: `HTTP_ONLY=ON` silently compiles it
out and makes the CM WSS setup fail before connection. The dependency build now
keeps HTTP(S), WS and WSS enabled while disabling unrelated protocols. Cross on
a supported game's details page starts Playden's existing background
`SteamInstallRunner`; it checks licenses and depot grants, then downloads,
verifies and commits depot content to `/data/PLDN10001/games/<app>`. This install
action has been exercised through the manifest stage. On PS4, `fstatvfs` returned
zero available bytes despite the console reporting 272 GB free; Playden now treats
PS4's volume-space query as unsupported and lets filesystem writes enforce actual
capacity. `ENOSPC` while creating or writing game files maps to the install's
no-space error, and the depot journal remains available for resume. The adjusted
download path still needs hardware validation.
Tokens remain memory-only and are wiped when the in-memory
repository clears or is destroyed. Persistent sign-in is not implemented; the
ordinary app data path is readable over FTP on a jailbroken console and must
not be described as secure storage. The paired SDK Vulkan triangle displayed
on hardware; its close-time crash is deferred per the user's assessment
because the dump is encrypted.

1. Add a `PLAYDEN_PLATFORM_PS4` target and a new PS4 title ID. Port Playden's
   UI display, controller, keyboard, networking, storage and logging adapters.
   Build a PKG and prove the signed-out library screen on hardware.
2. Build the orbis-win Wine runtime sources and PS4 Mesa/DXVK against the
   Playden title. Keep the tested ABI generation, relocatable eboot and GPU
   memory behavior. Package a versioned PS4 runtime kit with a complete file
   manifest, separate from the PS5 kit.
3. Add one title entry point with two modes: Playden UI and Wine game. Use
   `sceSystemServiceLoadExec` to restart the title into the selected game, then
   back into Playden when Wine exits. Pass only a short, validated game identity
   in the restart arguments; read the rest from Playden's committed game record.
4. Test an already installed game on hardware, including return, crash and
   restart behavior. This gate does not require a Steam account.

## Graphics port findings from orbis-win

The graphics paths in the reference are distinct; keep them that way in the
port plan:

* The native PS4 launcher in orbis-win uses `pw_videoout_ps4.c` plus the
  portable `pw_present` compositor. It allocates direct memory, registers
  three linear VideoOut buffers, draws into the buffer not being scanned out,
  and submits a flip. The title does not link RADV or Mesa for this UI path.
* OpenGL game support is the Windows WGL provider (`opengl32.dll` and
  `libgallium_wgl.dll`) using Zink over RADV. `scripts/build-app-ps4.sh`
  stages those DLLs separately from the native title. The Nov. 5 Half-Life
  handoff records the successful `Zink → RADV` run and the PS4 WSI display
  ownership fixes required before frames appeared.
* `src/wine-patches-ps4/0660-win32u-ps5-opengl-egl.patch` is explicitly the
  PS5 Wine EGL branch. It is not evidence that a native PS4 title can start
  Mesa EGL safely.
* The paired Orbis SDK's `examples/triangle` is the documented native Vulkan
  bring-up: `VK_EXT_headless_surface` reaches the PS4 display through Mesa's
  `wsi_orbis`. Its CMake uses the paired `vkloader`; `orbis-win`'s
  `scripts/radv-prx-ps4.sh` uses the same RADV, zlib, GNM Driver and VideoOut
  dependencies. The Vulkan sample is packaged under the existing `PLDN10001`
  title ID for iterative testing; the known-good VideoOut shell package stays
  in `build/ps4-shell/package/` as a rollback artifact.
* Mesa's PS4 WSI owns VideoOut when a Vulkan swapchain presents. orbis-win
  documented real `sceVideoOutOpen` busy errors when a second swapchain
  attempted to own the same output. A future Playden Vulkan UI must therefore
  have one clear display owner and hand off the current VideoOut presenter
  before opening its swapchain.

The earlier EGL experiment added static Mesa archives to Playden's eboot and
called `eglGetDisplay` during startup; that build crashed. The changed link
graph and initialization path were not isolated, so this does **not** prove
that Mesa or EGL alone caused it. The paired SDK Vulkan triangle has since
shown a frame on hardware. Its close-time crash is deferred per the user's
assessment; the encrypted dump is not actionable.

The native Hui backend is in `ps4/ui.cpp`. It creates Hui `DrawList` and
`Font` objects and rasterizes rounded rectangles, borders, capsule lines, and
baked SDF glyphs directly into the linear VideoOut buffer. The background uses
row fills instead of the per-pixel shape blender, and the pad loop polls at 8 ms
intervals to reduce input response delay. The current screen follows the
signed-out library layout in `src/homebrew/view.cpp`, with navigation across all
five tabs. Connect Steam starts QR sign-in and presents the challenge URL as a
QR code. After auth, the existing `LibraryClient` requests GetOwnedGames on a
worker and shows returned titles; this request still needs hardware validation.
Queue, Saves, and Settings remain empty-state screens. The PS4 transport
follows orbis-win's hardware-tested `libSceHttp`/`libSceSsl` path, with
certificate verification left enabled and HTTP/1.1 timeouts applied to each
request. It uses the console's certificate store rather than the app-bundled CA
file. Hardware confirmed HTTP 200 for the QR start and polling requests,
followed by `event=authenticated` and the signed-in UI. Steam initially blocked
phone approval because the phone VPN made its apparent location differ from
the console's; disabling that VPN allowed sign-in. Its sanitized trace goes to
klog and `/data/PLDN10001-network.log`; it records transport results, status,
timing, and byte counts without payloads, credentials, or header values.
Sessions are not persisted between title launches. This remains a shell
reproduction; it does not execute that view or Playden's simulation/services.
Mesh runs, images, clipping, shadows, glass, backdrop shaders, transitions,
and artwork are not implemented in this backend.

`src/homebrew/view.cpp` cannot be linked unchanged: `Render::Impl` depends on
`gfx::Renderer`, OpenGL textures, backdrop shaders, glass capture, and artwork
services. First validate this Hui-backed screen and controls on hardware, then
extend the backend only for primitives the signed-out/library view needs. This
keeps the launcher as the sole VideoOut owner and leaves Vulkan/RADV for the
future Wine game mode.

## Milestone 2: local game preparation

**First target: Half-Life 2 (Steam app 220).** orbis-win's hardware-tested
Half-Life 2 path reaches the menu, loads a map and exits to its launcher. Its
reference configuration supplies the settings to translate into Playden's
own per-game launch record: `hl2.exe`, working directory `C:\Games\HalfLife2`,
`-game hl2 -novid -fullscreen -w 1920 -h 1080`, 32-bit DXVK 2.6.2, and the
tested XInput/touchpad mapping. Do not package or depend on GLDS10043 or its
profile file. The same orbis-win handoff identifies the needed Wine file
mapping, WoW64 and frame pacing fixes; pin those in the Playden runtime kit.

1. **Hardware confirmed:** fetched Steam app details for app 220. The console
   reported a Windows launch option and 12 Windows depots. License and depot
   access still need confirmation for the signed-in account during install.
2. **Built; hardware validation pending:** Cross on game details starts
   Playden's existing resumable depot installer. It targets
   `/data/PLDN10001/games/220`; the installer commits the install record only
   after depot content has been verified and applied. Verify license checks,
   CDN transfer, cancellation/resume and the committed record on the console.
   The PS4 volume-space preflight is disabled because the app's query reported
   0 bytes while the console showed 272 GB free; actual file writes report
   `ENOSPC` and preserve the installer journal.
   The installer crash was traced to libc++ comparing pthread's FreeBSD
   `ETIMEDOUT` value (60) against the SDK header's 9938. The PS4 build now
   compiles POSIX timed waits with the FreeBSD value so normal worker polling
   timeouts do not become uncaught `std::system_error` exceptions.
   The resumable installer is stable with one download connection after changing
   chunk decryption to reuse the encrypted response buffer. Two and four
   connections still exhaust the title's C/C++ allocation path during concurrent
   one-megabyte chunks: two workers failed while allocating decoded 1 MiB buffers,
   and four workers failed while allocating a roughly 894 KiB response buffer.
   At the same time, the console reported about 432 MiB of flexible memory and
   several GiB of direct memory available, so this is not physical memory
   exhaustion. The direct-memory pool cannot serve `malloc`, and the remaining
   investigation must isolate heap growth, virtual address layout, fragmentation,
   and allocator concurrency in the PS4 compatibility layer.

   **Resolved in 01.86.** The cause was libSceLibcInternal's heap, fixed at
   13,712 KiB on PS4 regardless of the eboot's heap settings (measured with
   `malloc_stats_fast` in 01.85). The UI module now allocates from its own
   heap (`ps4/playden_heap.c`, via `-Wl,--wrap`), backed by flexible memory,
   and downloads use four connections again. The notes below are kept for the
   remaining acceptance run.

   **Original plan: PS4 concurrent-download memory.** Keep one connection as the
   supported default until this is resolved. Add allocation telemetry around the
   native HTTP response and decoded chunk lifetimes, verify how musl grows its heap
   from flexible memory, and bound the downloader's aggregate live chunk memory.
   Consider a fixed buffer pool or a fetch/decode pipeline with an explicit memory
   budget so network concurrency cannot start allocations that the decoder cannot
   finish. Completion requires a full 2.8 GB Half-Life 2 install at four connections
   on hardware, including interruption and resume, with no allocation failure and
   no invalid completed-install record.

3. Build a clean prefix template with the same pinned host Wine revision as
   the PS4 runtime. The PS4 title cannot spawn `wineboot` as a separate process;
   Playden must copy and specialize this template for each game.
4. Add the pinned Wine/DXVK runtime kit and generate launch settings from
   Playden's Steam app details and install record. Verify the executable,
   directory, arguments and architecture before launching.
5. Run the installed game in the same PLDN10001 title through the orbis-win
   `sceSystemServiceLoadExec` handoff, returning to Playden after Wine exits.
   Confirm HL2 reaches its menu, loads a map and exits cleanly on the console.

**Second target: Left 4 Dead 2.** The orbis-win hardware run found a model decal
crash and verified `r_drawmodeldecals 0` as a workaround for at least 9m30s.
Keep that setting scoped to L4D2's generated launch/config data; validate it
after the HL2 path is stable.

## Milestone 3: Steam workflow

Enable the existing Steam sign-in, library, downloads, local import, saves and
updates through PS4 platform adapters. Validate interrupted downloads, low
storage, kit mismatch, sign-out and save conflicts on hardware. Add optional
per-game Home tiles only after the single-title flow is stable.

## Source boundaries

* Playden owns user data, game records, installation and the UI.
* orbis-win contributes the PS4 runtime and hardware fixes; `GLDS10043` is a
  standalone test application, not a runtime dependency for Playden users.
* Keep Playden's PS5 build and kit pin independent of the PS4 build and kit.

References: [Playden source](https://github.com/leolobato/playden-ps5),
[orbis-win source](https://github.com/plastikman/orbis-win),
[orbis-win Wine fork](https://github.com/plastikman/prospero-win).
