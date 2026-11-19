# Handoff: PS4 kit capability parity

Date: 2026-11-18. Plan: `/home/zpeirce/.claude/plans/wobbly-hopping-panda.md`
(approved, phases 0-7). Phases 0-1 were initially the stop point; work has since
continued through source implementation for phases 3-6. Latest evidence is
below.

## Baseline correction — the last known-working Orbis-Win is later than 02.15

Do not use 02.15 as the latest known-working Orbis-Win baseline. The 02.15
stage is only the older comparison base used while constructing the 02.17
profile candidate. The 2026-11-17 handoffs record a later fresh-source build:

- Package marker: `GLDS10043-fromsource-1117`.
- Installer: `IV0000-GLDS10043_00-GLDS100430000000.pkg`.
- SHA-256: `ef598a8376e190e28ad860d970623c3190d321bad4d6545c6ee6340dd83f89ce`.
- It passed the parity gate and booted on hardware. The accompanying
  `-vkbun` hardware run records Fallout: New Vegas at 59.8 fps and Half-Life 2
  at 59.9 fps, with zero libvulkan faults. The `-vkbun` package is a distinct
  artifact (`pkgout-vkbun`, SHA-256
  `ee014b12302397e5e8f7cd918ddf820267c8b9c69e73c4dfb7b16bb24efd292c`), so
  do not conflate it with the `fromsource-1117` package hash above.
- `02.59` is a compatibility/parity reference in the from-source handoff, not
  the version number of the fresh-source package.

When comparing later failures, start from the exact fresh-source artifact and
its recorded input manifest. The 02.15 package remains useful only for the
specific 02.17 component-preservation comparison; it is not the top-level last
known-working baseline.

## 2026-11-18 continuation: Playden 01.26 install attempt

- After the console rebooted, a Remote Play snapshot showed the PS4 home screen
  with Playden selected. The captured frame is retained at
  `/home/zpeirce/junk/codex_scratch/ps4-current.png`.
- Re-ran `check-ps4-package-parity.py` against the current Playden 03.54 stage
  and the exact 01.26 candidate, with `--elf-dir` set to the PRX build that
  matches the candidate's `ntdll.prx` and `wineserver.prx`, and
  `--upstream-root` set to the committed Prosper-Win source. All required
  provider, export, diagnostics, signal-mask, and baseline-file checks passed:
  **0 FAIL, no warnings**. An earlier invocation using the Playden `prx`
  directory produced warnings because that directory did not contain the
  matching runtime PRX build; it was discarded and the corrected invocation
  above is authoritative.
- Rechecked package SHA-256:
  `5b32558b72f7f1d316c284beb56fa6481ef06ffc74d490d92115f37e40ded135`.
  Existing `pkg_validate.txt` contains all `[OK]` digest and signature checks.
- The user authorized installing this exact 01.26 package. The MCP installer
  could not use Web File Manager because port 8888 was down. GoldHEN FTP was
  enabled through GoldHEN > Servers Settings > Enable FTP Server. Before
  upload, `/data/pkg/IV0000-PLDN10001_00-PLDN100010000000.pkg` had size 0/was
  absent. The repo's minimal no-login `ftp-put.py` then transferred the full
  package; `ftp-size.py` reported 1,547,501,568 bytes and a streamed FTP RETR
  SHA-256 matched the local package exactly:
  `5b32558b72f7f1d316c284beb56fa6481ef06ffc74d490d92115f37e40ded135`.
- GoldHEN Debug Settings > Package Installer listed this exact package as
  `Playden for PS4 - PLDN10001 v01.26 [gd]`. The console asked to confirm
  reinstall because the same content was already installed; the user had
  authorized the reinstall. Installation completed and returned to the package
  list. Playden was launched and reached its Library UI.
- The UI currently says **Steam not connected** and shows zero games. The
  receiver is listening at `10.168.2.188:9300` from
  `/home/zpeirce/junk/codex_scratch/ps5logd-runner/ps5logd.py`; no Wine/game
  startup lines have arrived because no game could be launched. Screenshots:
  `/home/zpeirce/junk/codex_scratch/ps4-package-installer.png`,
  `/home/zpeirce/junk/codex_scratch/ps4-installed-playden-0126-progress2.png`,
  `/home/zpeirce/junk/codex_scratch/playden-0126-home.png`.
- Next hardware step is to connect Steam in Playden, wait for the library to
  populate, then run the plan's one-setting-at-a-time title tests while
  retaining the active log reader. Do not interpret the current zero-game UI
  state as a runtime failure.

## Latest package build update — 2026-11-18

The previously blocked source and package work has now produced two local,
validated test packages. Neither has been uploaded or installed.

### Orbis-Win 02.17 test runtime

- Installer: `/home/zpeirce/junk/codex_scratch/runtime-kit-parity-build-20261118/orbis-profile-0217/app/IV0000-GLDS10043_00-GLDS100430000000.pkg`
- SHA-256: `4f9d5b02f034e3146b2371d4248fb385e73b52900eae1ef5b5d309740d4b45c4`
- Source commits: Orbis-Win `db657ef7c27542ee6793be093f7961a22ee40953`; Prosper-Win `8604f3b6c38cc8eb273bc8f2079a870e5482ba98`. Both tracked trees are clean.
- Includes profile environment, audio-volume, and Steam install-path code; `kernelbase32.dll` carries 25 `PW_EXCEPTION32` strings. Test logging is enabled to `10.168.2.188:9300` through `/app0/dev.conf`.
- The 02.15 ABI-v4 `wow64native.dll`/PRX pair and RADV PRX were preserved byte-for-byte. Zink was carried from the 02.16 stage. Mesa source is clean at `0ec293c29f9735ee462eff0d9a9316481d741afa`.
- Parity against the known-good 02.15 stage: **0 hard failures, 0 baseline files absent**. `PkgTool.Core pkg_validate`: all hash/signature checks `[OK]`.
- A fresh Mesa compile could not run because the local Nix store lock is inaccessible (`Permission denied`). This package uses the known-good 02.15 RADV binary, not the 02.16 driver that crashed the host title.

### Playden 01.26 test package

- Installer: `/home/zpeirce/junk/codex_scratch/runtime-kit-parity-build-20261118/playden-profile-parity-0126/package/IV0000-PLDN10001_00-PLDN100010000000.pkg`
- SHA-256: `5b32558b72f7f1d316c284beb56fa6481ef06ffc74d490d92115f37e40ded135`
- Built from Playden `sync/v0.12.0` with Mono 11.3.0, DXVK 2.6.2, and vkd3d-proton 3.1 staged. The kit view advertises **17 capabilities**. `vkd3d-proton` remains unqualified and is not advertised. Test logging targets `10.168.2.188:9300`.
- Parity against the 03.54 stage: **0 hard failures, 0 baseline files absent**. `PkgTool.Core pkg_validate`: all hash/signature checks `[OK]`.
- This establishes package/provider parity, not hardware qualification. No title has been run on this package.

### Console qualification still needed

1. Install the exact PLDN10001 package above. Confirm Playden starts and the reader at `10.168.2.188:9300` receives the build marker and launch logs.
2. Run known-good regression titles (FNV, HL2/Portal, and Mafia) and check rendering, audio, input, clean exit, and return to Playden.
3. Exercise the new profile settings individually: a profile environment override, reduced audio volume, and Steam `install_dir` lookup. Confirm the values appear in runtime logs and Steam library/install paths remain intact.
4. Run a 32-bit title through the hosted-driver path and verify the expected 1076 startup/attach diagnostics before treating `wine-driver-threads` as hardware-qualified.
5. Run a .NET title to qualify Mono. D3D12/vkd3d remains explicitly unclaimed until a D3D12 game renders successfully.

No upload, install, or GitHub push has occurred during this continuation. The
user's latest authorization covered local commits only.

Repo: `/home/zpeirce/junk/playden-upstream-sync-1118/trial`, branch
`sync/v0.12.0`. Four local commits sit unpushed:

| commit | what |
| --- | --- |
| `1b2c8329` | web: wait for the socket to drain instead of abandoning the response |
| `42fdaab2` | PS4: claim the capabilities the runtime's profile parser already reads |
| `80f2e516` | PS4: derive kit capabilities from evidence, and test that they stay honest |
| `7f1020b3` | PS4: gate kit capabilities on staged runtime evidence |

Push destination is **unresolved** - `sync/v0.12.0` already merged to
`plastikman/playden-ps4` main via PR #4, so this needs either a push to main or a
second PR. Ask before pushing.

## Why this work exists

Playden gates almost every per-game setting on the kit manifest's `capabilities`
array (`src/runtime/kit_trust.cpp:13`, 34 names;
`KitCapabilitiesFromManifest()` at `src/runtime/game_settings.cpp:4187`).

Leo's kits cannot be reproduced: they are an incremental chain of binary copies
(kit A -> D/F -> H r1..r9 -> 0.10), each `update_rN.py` rewriting one or two files
on a copy of the previous kit's inputs, rooted in artifacts `tools/kit-h/README.md`
says are "not in this repository". That README calls itself "a record. **not** a
turnkey build yet". So parity can only mean matching *declared capabilities and
the behaviour behind them*, never matching his artifact.

Ours were hand-declared in `scripts/ps4-kit-view.py` and drifted both ways:
`opengl-zink` unclaimed while PS4 shipped the provider (Binding of Isaac rendered
nothing, 2026-11-12); `profile-version-2` unclaimed (MDK could not reach the
translator); `game-root-unowned` claimed wrongly (launcher crash, 03.38).

## Current artifacts and claims

The existing Playden `01.25` candidate advertises 11 capabilities. A feature-
enabled staged-kit simulation advertises 17, including patch 1076's compiled
driver-thread providers; vkd3d-proton remains unqualified and unclaimed. No
complete feature-enabled Playden package has been built or installed.

## What is on the console

No feature-enabled Playden package from this work is on the console. Current
console testing used Orbis-Win `GLDS10043`; the latest Remote Play result and
the separate FNV guest fault are recorded below.

## Phase 1, as built

`scripts/ps4-kit-view.py` now carries two tables:

- `CAPABILITY_EVIDENCE` - each claim paired with either staged provider files that must all exist, or `()` plus a note of the profile key the runtime's parser reads. A claim whose provider is missing is dropped rather than shipped.
- `NEVER_CLAIM` - each refusal with its reason, in code, so a later audit does not re-derive it.

`tests/test_ps4_kit_view.py` (13 tests; current run passes).

**Subtlety worth keeping.** The test reads the vocabulary from `game_settings.h`
(`kKit*` constants), **not** `kit_trust.cpp`. That list filters a *PS5* kit in
`kit_manifest.cpp:281`, which drops an unknown name into `ignored_capabilities`.
The PS4 path never goes through that validator - `kit_ps4.cpp:184` pushes every
string straight into `kit.capabilities` and `runtime_host.cpp` hands it to
`KitCapabilitiesFromManifest`, which compares strings directly. That is why
`input-touchpad` and `input-keyboard-stick` work here although they are absent
from `kit_trust.cpp`; a PS5 kit declaring them would have them ignored. The test
pins exactly that pair so the difference stays a decision on record.

## Phase 2: a finding that changes it

Phase 2 was to claim `wine-shared-sections` and `wine-driver-threads`, which the
audit found present as patches `1024` and `1076`. **Only one survives contact with
the build**, and this is the reason `AGENTS.md` says `src/wine-patches-ps4/` is an
archive, not a build input - the trees come from pins in `src/wine-trees/`.

Measured against the shipped `build/ps4-runtime/pkg-stage/sce_module/ntdll.prx`:

| patch | marker | result |
| --- | --- | --- |
| 1024 | `"unaligned committed mapping %p-%p, using private copy"` | **present** |
| 1076 | `"hosted driver thread %04lx: no thread attach"` (ascii), `L"...syswow64\drivers\"` (utf-16) | **absent**, both |

Control: `ntoskrnl` appears once, so the binary is the right one and the search
works. So `wine-shared-sections` is evidenced and claimable;
**`wine-driver-threads` is not in this build and must not be claimed** on the
strength of the patch file existing. Before Phase 2, establish which tree pin the
runtime was built from and whether 1076 is in it.

Note the stage was built with `ORBIS_WIN_RELEASE=/home/zpeirce/junk/playden-pin-1118/orbis-win-profilever`,
a pinned release, which is very likely why the archive and the binary disagree.

## Remaining phases (detail in the plan)

3. `[environment]` - the keystone, unlocks 3. `wine-large-address-aware` and `environment-file-budgets` are **already implemented in Wine** (patches 1040, 1056 read the variables) and merely unreachable: `debug_env_field()`'s allowlist (`pw_game_profile.c:348`) has no `WINE_*`. The injection path already works - `[debug] env` *is* it.
4. `[audio] volume` - `pw_audio_ps5_open_port()` sets eight channels to 0dB unconditionally (`native/pw_audio_ps5.c:53-54`); scale them.
5. `wine-mono` - staging + template registry.
6. `[steam] install_dir` - parser plus a bottle-side steamapps tree.
7. `vkd3d-proton` - largest; qualify against our RADV first.

Target 9 -> 18 of 34.

## Do not claim, and why

- `display-120hz` - Playden's `SoftwareFrameLimit()` **disables** its software limiter for a kit claiming it; our runtime has no `[display] frame_rate` pacing, so games would run uncapped. `[display] refresh` taking 120 is not evidence.
- `display-vrr`, `settings-dxvk-present` - every present is an `ORBIS_VIDEO_OUT_FLIP_VSYNC` flip at 60 Hz. `display-vrr` additionally has no `KitCapabilities` field at all and flips a **PS5** `param.json attribute3` bit on every title, invalidating them.
- `bottle-profile-shared` - admission requires **exactly 0700**; claiming triggers a chmod sweep to 0711 across all installs, irreversibly. (An earlier note in this session that PS4's native 0711 made this promising was **wrong** - that comment was Playden's reader, not our shell's gate.)
- `publication-open-modes` - sets `strict_modes = 0` in Playden's own pre-flight, so a title reports admissible while the shell refuses it at launch.
- `helper-process` - **owned by another session**, branch `codex/ps4-local-process-probe` in `/home/zpeirce/git/playden-ps4`. See `docs/HANDOFF-2026-11-18-PS4-NATIVE-STEAM.md`. PS4 *can* create local children (`sceSystemServiceAddLocalProcess` works); the blocker is `SCM_RIGHTS` failing `EINVAL` plus a separate child descriptor table, so the child cannot receive the Wine-server socket the PS5 helper's `SendFd()` design needs. `title-notify` is downstream of it.
- The rest are in `NEVER_CLAIM` in the script with their reasons.

## Other loose ends

- Upstream PR mpereiraesaa/prospero-win#753 (`[runtime] version`) is open; `[environment]` and `[audio]` are candidates for the same route.
- orbis-win PRs #21/#22 are closed.
- `~/git/playden-ps4` still on `ps4/upstream-ports`.
- `relocated_wine_app.prx` carries +518 strings from a shared `.deps`; not for a release build.

## Continued work and current state (2026-11-18)

### Worktrees

- Playden: `/home/zpeirce/junk/playden-upstream-sync-1118/trial`, branch
  `sync/v0.12.0`. Three earlier commits remain unpushed, with additional
  uncommitted builder/evidence changes. Preserve untracked `deps` and
  `scripts/network-deps/.deps` symlinks.
- Prosper-Win: `/home/zpeirce/junk/codex_scratch/prospero-profile-env-20261118`,
  branch `codex/profile-environment`. Environment, audio, and Steam changes are
  uncommitted; `.deps` is a scratch overlay symlink. The original source trees
  and `/home/zpeirce/git/orbis-win` have unrelated dirty files; leave them alone.

### Implemented in source, not yet released

- `[environment]` profile parser allows Wine variables through Playden's
  existing extra-environment path; capacities were raised and tests cover it.
- `[audio] volume` parser and per-channel PS5 audio scaling are implemented.
- `[steam] install_dir` parser validates one safe path segment. Before launching,
  the runtime safely writes the game's root into the bottle's
  `C:\Program Files (x86)\Steam\steamapps\common` virtual `.pw-symlinks`
  table. No hardware title lookup has been verified.
- Playden's builder optionally stages official Wine Mono 11.3.0, writes the
  template bottle's matching `RuntimePath`, and records version/hash. No .NET
  title has been tested. `WINE_MONO_VERSION` is exported so the kit-view checker
  validates the same version the builder stages.
- `ps4-kit-view.py` now ties environment/audio claims to the compiled PRX marker
  `PW_WINE64 profile_features=environment,audio-volume,steam-library-path`.
  This stops an old runtime release from claiming parser additions that exist
  only in the source checkout. Steam additionally requires its own compiled
  handler marker. Mono requires payload and exact registry path.
- The builder still claims `wine-driver-threads` only if the actual staged
  `ntdll.prx` has patch 1076's marker. It is absent from the currently selected
  runtime. `wine-shared-sections` is present.

### Verification and blockers

- Playden `python3 tests/test_ps4_kit_view.py`: 13 tests pass; Python compile,
  shell syntax, and `git diff --check` pass.
- Prosper-Win focused `test_pw_game_profile`, `test_pw_audio_ps5`, and
  `test_pw_wine_start` pass. An isolated PS4 link of the updated app ELF
  contains the feature marker.
- Built and validated a full Orbis-Win `02.15` candidate from the isolated
  `codex/profile-environment` source tree, using the existing ABI-v4 runtime
  modules and their matching DLL/PRX pair. Installer:
  `/home/zpeirce/junk/codex_scratch/runtime-kit-parity-build-20261118/orbis-profile-0215/app/IV0000-GLDS10043_00-GLDS100430000000.pkg`,
  SHA-256 `c302a84733041ae46d97c0b1655ff99b59a65647f6ee2321f6dfd2e6aba765db`.
  Its `relocated_wine_app.prx` contains
  `PW_WINE64 profile_features=environment,audio-volume,steam-library-path`.
  `PkgTool.Core pkg_validate` passed all digest/signature checks, and the three
  focused Prosper-Win tests passed. This is an uncommitted local build; it was
  not uploaded or installed.
- Do not run the Playden package parity gate on the Orbis-Win stage alone: it
  correctly reports missing Playden UI/assets and Mesa Zink, which Orbis-Win
  does not package. Build the Playden stage from this candidate first, then
  compare that complete stage against `03.54`.
- The Playden builder requires a clean, committed Orbis-Win/Prosper-Win source
  revision in `BUILD-MANIFEST.txt` and verifies the staged app PRX against that
  manifest. It intentionally refuses a dirty source diff. No commit or push was
  made; after commit authorization, build through the ABI-v4 wrapper so the
  manifest, package hashes, and Playden provenance agree.
- The ABI-v4 package scripts pinned a different `wow64native` pair than the
  selected release. In a separate Orbis-Win worktree,
  `/home/zpeirce/junk/codex_scratch/orbis-profile-parity-20261118` on
  `codex/runtime-kit-profile-parity-20261118`, I updated the defaults in
  `scripts/build-app-ps4.sh`, `scripts/ci/build-abi4-package.sh`, and
  `scripts/build-orbis-ps4-fast.sh` to the pair recorded by the selected
  release manifest: DLL `931070ef2a2fb4ab7b74fd7ae2c991900577d5181e128e4174f73b3a0ae96f84`,
  PRX `700f96c6540b818b80f18a49b44232927a24524eedebb620f113b39ee9c0f6d5`.
  Shell syntax and `git diff --check` pass. These changes are uncommitted and
  have not been built through the CI wrapper yet.
- A parity invocation on the bare Orbis-Win `02.15` stage returned one expected
  failure because that package does not contain Playden's Mesa Zink provider,
  plus a warning that 1,916 Playden UI/assets are absent. This was the wrong
  package scope for the Playden gate, not evidence that the Orbis-Win build is
  installable as a Playden package. The `01.25` complete Playden stage remains
  the only candidate with a passing Playden parity result.
- A complete Playden `01.25` candidate package was built on 2026-11-18 in
  `/home/zpeirce/junk/codex_scratch/runtime-kit-parity-build-20261118/playden-mono-vkd3d/`.
  Installer: `package/IV0000-PLDN10001_00-PLDN100010000000.pkg`, 1.5 GiB,
  SHA-256 `7737517e4565d74202b15cf726f1503bcb2a5f40c2f752fad54fce2493dfdc87`.
  It contains Mono 11.3.0 and vkd3d-proton 3.1; vkd3d remains explicitly
  unqualified (`vkd3d_proton_qualified=0`). Kit view reports 11 capabilities,
  adding `wine-mono` to the prior 10. It does **not** claim environment, audio
  volume, or Steam library path: the staged `relocated_wine_app.prx` is still
  the released Orbis-Win payload and lacks the source feature marker.
- The mandatory parity check passed against the `03.54` stage at
  `/home/zpeirce/junk/playden-upstream-sync-1118/trial/build/ps4-runtime/package/pkg-stage`.
  The first invocation used the wrong `--elf-dir` and skipped some checks; the
  corrected invocation used
  `/home/zpeirce/junk/orbis-fromsource/prospero-win-abi4/.deps/wine-ps5/prx`
  whose `ntdll.prx` hash matches the candidate. The corrected run passed all
  gates: ntdll/wineserver exports, guest fault diagnostics, FreeBSD signal-mask
  checks, required provider files, and no baseline payload omissions. No
  warnings or failures remained. This is a parity result, not hardware
  qualification; the package has not been uploaded or installed.
- Earlier attempt failures around ABI-v3 `wow64native.dll` remain relevant only
  to building a new Orbis-Win runtime from source. The `02.15` candidate uses
  the ABI-v4 DLL/PRX pair from the selected release stage.
- Official vkd3d-proton v3.1 archive downloaded to
  `/home/zpeirce/junk/codex_scratch/vkd3d-proton-3.1/`. Its x86 DLLs identify as
  PE machine `0x014c`; x64 DLLs as `0x8664`. The DLLs are staged in the `01.25`
  candidate, but the capability remains unclaimed; only a D3D12 hardware
  render can qualify it.
- Playden's build path now supports optional vkd3d-proton staging via
  `VKD3D_PROTON_DIR`, checks all four PE architectures, and records the supplied
  version and payload hash. `VKD3D_PROTON_QUALIFIED` defaults to 0; only setting
  it to 1 after the hardware render test can advertise the capability. The
  test verifies staged files alone do not enable it.

  v3.1 archive SHA-256: `b85ea8219b71093a3924913dd7aae6172333dc0c069983ed510816d67f02120b`

  | DLL | PE machine | SHA-256 |
  | --- | --- | --- |
  | x86/d3d12.dll | 0x014c | `e01ca92c677c494012d0ab38612e049ef70ed393ecf5f804cd48be6dac507ef2` |
  | x86/d3d12core.dll | 0x014c | `deaecb86b3ef91ed01ddcff63d1c2ac160af3a96b009f1571f79af334ac55248` |
  | x64/d3d12.dll | 0x8664 | `502694a5fd5e06381ef2cdb33359c5948b6a012938902ddc9e9b381841bc0ce7` |
  | x64/d3d12core.dll | 0x8664 | `0dfd690a39ef8361ca3b64e9fd8e945b27d2c568dac5dc0dce1a45abf65914cc` |
- Wine Mono is extracted under
  `/home/zpeirce/junk/codex_scratch/wine-mono-extract-11.3.0/` and included in
  the `01.25` candidate.
- Orbis-Win `02.16` was built after bringing the hosted-driver PE changes into
  a separate host-runtime overlay. Patches 1043 and 1076 were applied in the
  clean host Wine source branch `codex/driver-threads-hostbuild-20261118`;
  i386 and x86_64 `ntdll.dll` compiled with the no-thread-attach marker, and
  both `ntoskrnl.exe` builds compiled with the hosted-driver marker. The
  generated `wine.inf` also contains the hosted-driver service entries.
- The Playden kit-view checker had incorrectly searched for the Windows PE
  `ntdll.dll` marker inside `sce_module/ntdll.prx`. It now requires the two
  architecture-specific `ntdll.dll` and `ntoskrnl.exe` files and every compiled
  marker before advertising `wine-driver-threads`. The 13 targeted kit-view
  tests pass. The candidate Playden `01.25` stage predates the fix and must be
  rebuilt from Orbis-Win `02.16` before it can claim this capability.
- Orbis-Win `02.16` installer:
  `/home/zpeirce/junk/codex_scratch/runtime-kit-parity-build-20261118/orbis-driver-threads-0216/app/IV0000-GLDS10043_00-GLDS100430000000.pkg`
  SHA-256 `6039720532da94a6e0bcce025b96c212a0ac35f73dc98bc3fc3911ece47e2ee3`.
  `pkg_validate` passed all package/signature checks. The mandatory parity
  script reported 0 hard failures; it warned that `dev.conf` is absent and
  that one PS4-specific RADV string differs from the baseline. Those warnings
  are recorded in the build output and should be reviewed before promoting a
  release. The four hosted-driver PE markers are present in the candidate
  stage.
- At the user's explicit request, the `02.16` installer was uploaded to the
  console's `/data/pkg/IV0000-GLDS10043_00-GLDS100430000000.pkg`; GoldHEN FTP
  confirmed the full 542,441,472-byte transfer with `226`. The user installed
  it and launched Fallout: New Vegas. This is **Orbis-Win** (`GLDS10043`), not
  Playden (`PLDN10001`).
- Hardware result for `02.16`: FNV stayed at the spinning dots. The Wine log
  reports `frames_put=0` throughout. At 17:36:47, guest thread `0024` took a
  user-mode access violation in the exact packaged `libvulkan` at
  `libvulkan.so+0x16a8f00`, fault address `0x21c565510`. The matching
  `libvulkan.shared.elf` resolves the PC to `regexec`; the faulting instruction
  is a compare through a computed pointer. The process remained alive and then
  repeatedly sampled at the same guest wait state; this was not a PS4 system
  crash. The user asked Codex to take control; Remote Play captured the spinner,
  then navigated home and closed the stalled title after evidence was saved.
  The post-close launcher image is also preserved as `remoteplay-after-close.jpg`.
- Run evidence is under
  `/home/zpeirce/junk/codex_scratch/runtime-kit-parity-build-20261118/fonv-run-20261118/`:
  `ror2-sync-trace-20261118-131500.log`,
  `ps4-klog-20261118-133349-watch.log`, and `remoteplay-latest.jpg`.
  Build marker: `GLDS10043-02.16-pwa8f88505b5+dirty`. Package SHA-256:
  `6039720532da94a6e0bcce025b96c212a0ac35f73dc98bc3fc3911ece47e2ee3`.
- Do not attribute the FNV regression to patches 1043/1076 yet. The 02.15 to
  02.16 package comparison also changes `libvulkan.prx` (24,862,400 to
  24,859,536 bytes), `ws2_32.prx`, both PE architecture sets, and other runtime
  inputs. The 02.16 build manifest leaves `mesa_commit` blank, so exact Vulkan
  source provenance is missing. The 02.16 fault is proven; its cause is not.
  The older `libvulkan.prx` SHA-256 is
  `268fdd06f1c9fa724db6f68124d28dafaa9b05df90a35e907e30158dd23727df` and is
  byte-identical across the retained 02.15 candidate, Playden `01.25`, and
  `~/junk/orbis-fromsource/pkgout-vkbun`; the 02.16 binary is
  `8520f5cfecda009ed79b61c6fd01747357d1b70e60215ec701c0fd5bf5037630`. This
  makes the rebuilt Vulkan PRX a priority for the controlled A/B, but does not
  yet prove it caused the hang. The post-close Remote Play screenshot is
  `remoteplay-after-close.jpg` in the same run evidence directory.
- Follow-up Remote Play check at 18:29 AKST: the PS4 home screen showed the
  Orbis-Win test title (`GLDS10043`) selected. Opening it showed the Prosper-Win
  library with Fallout: New Vegas visible. Pressing Cross from that screen led
  to a PS4 system error `CE-34878-0` naming **Orbis Wine Driver Thread Parity**;
  this is a host-title crash report, not evidence that the FNV guest reached its
  previous spinner or faulted at the same instruction. Screenshots are in
  `fonv-run-20261118/rerun/after-launch-15s.jpg`, `after-ps-button.jpg`, and
  `power-menu.jpg`. The PS4 klog reader was stale, so a fresh reader was
  started; it connected to port 3232 and captured shell memory-status lines,
  but no `GLDS10043` or Wine application messages. The raw capture is
  `fonv-run-20261118/rerun/klog-live.log`. The existing 02.16 guest
  `libvulkan` fault remains the only symbolized FNV signature.
  Do not treat this rerun as a successful A/B or infer a root cause from the
  CE code alone.
- No feature-enabled Playden candidate was uploaded or installed. The existing
  `01.25` Playden package remains a baseline candidate; Mono and the profile
  capabilities are not hardware-qualified, and vkd3d-proton remains unqualified.
- User-requested package cleanup removed 28 superseded/duplicate `.pkg`
  artifacts across `~/junk` plus four ignored repository build/output paths,
  freeing 13.61 GiB. The complete
  SHA-256/size/path deletion manifest is
  `/home/zpeirce/junk/codex_scratch/package-cleanup-2026-11-18.tsv`.
  Source trees, crash data, official release artifacts, console readbacks, and
  retained baselines were preserved. The only `.pkg` remaining elsewhere in
  the home tree is the console readback at
  `/home/zpeirce/.cache/codex-splinter/console-before-20261118.pkg`. Current
  free space after the candidate build is approximately 13 GiB.

### Next actions

1. After local commit authorization, commit the reviewed profile changes on
   the isolated Prosper-Win branch and the ABI-v4 pin correction on the
   isolated Orbis-Win branch. Then run Orbis-Win's ABI-v4 wrapper so it writes
   a clean, exact `BUILD-MANIFEST.txt`.
2. Build Playden from that Orbis-Win release stage. Confirm the compiled marker
   gates `settings-profile-environment`, `settings-audio-volume`, and
   `steam-library-path`; keep unverified features unclaimed.
3. Run the mandatory Playden package parity check against `03.54` with the
   exact matching PRX build directory and upstream source. The Mono/vkd3d
   `01.25` package passed this gate; the profile-enabled Playden package does
   not exist yet.
4. Hardware-qualify Mono with a .NET title, environment/audio/Steam profile
   behavior, and vkd3d-proton with a D3D12 title before enabling further
   claims. The current Mono/vkd3d package has not been uploaded or tested.
5. Isolate the `02.16` regression before further Orbis runtime changes. First
   recover the exact 02.15 build provenance and compare its `libvulkan.prx`
   source/build inputs with 02.16; then A/B the relevant runtime inputs one at
   a time. Do not claim hosted-driver patches caused the fault based on this
   run. For hardware comparison, use the exact installer filename above and
   preserve known-good fixes.
6. After isolating that regression, test a known-good 32-bit title that
   exercises the hosted-driver path. Capture the runtime build marker and
   verify the logs show hosted-driver startup plus the 1076 `no thread attach`
   diagnostic. Check input, gameplay/render/audio, clean exit, and return to
   launcher. A successful 32-bit run alone is not enough to claim Playden's
   kit capability; rebuild Playden and confirm the kit view lists it only when
   all four PE binaries and markers are staged.
7. Review the `dev.conf` and RADV parity warnings, then rerun the parity gate
   if either warning indicates an unintended omission.

Nothing from this continuation has been committed or pushed to GitHub. The
three prior Playden commits and all dirty worktrees must be preserved. The
02.16 package was uploaded and installed. The user tested FNV, and Codex used
Remote Play to close it after preserving the stalled run's screenshot and logs.
No Playden package has been installed.


### Current branch / capability-view update (2026-11-18)

- `git ls-remote` and a fetch confirmed `origin/main` at `2a113bfd` (merge of
  PR #4). The three commits `1b2c8329`, `42fdaab2`, and `80f2e516` are not in
  remote `main` or the remote sync branch. A new PR against current `main` is
  appropriate; nothing has been pushed.
- The current Playden `01.25` stage advertises 11 capabilities. Its view is
  correct for its providers: nine baseline claims plus `wine-mono` and
  `wine-shared-sections`; it predates the hosted-driver PE markers and new
  profile runtime PRX.
- A scratch-only capability-view simulation combined the exact Orbis-Win
  `02.16` app/PE modules with the staged Zink providers and Mono template. The
  checker returned 17 capabilities with vkd3d unqualified. It omitted no
  baseline provider in this simulation. This is not a complete Playden stage,
  package, or hardware test. The four 1076 markers were verified in both PE
  architectures of `ntdll.dll` and `ntoskrnl.exe`.
- Playden's real package builder refuses the `02.16` release manifest because
  `prospero_win_abi4_diff_sha256` is nonempty; the manifest also has an empty
  `mesa_commit`. Do not bypass either check. A committed, exact-provenance
  Orbis-Win release stage is required for a complete feature-enabled Playden
  package. Local commits are awaiting explicit authorization.
- Current tests rerun: Playden kit-view suite 13/13; Prosper-Win
  `test_pw_game_profile`, `test_pw_audio_ps5`, and `test_pw_wine_start` pass.
- Release provenance guards are now prepared in the two local source trees:
  `build-abi4-package.sh` records the Orbis-root diff, hashes ABI-v4 diffs
  against `HEAD` (so staged edits count), and requires the actual Mesa checkout
  to match `src/mesa-trees/mesa-ps4-HEAD.txt` with no tracked edits. Playden's
  builder verifies the Orbis root commit/diff, linked Prosper-Win commit/diff,
  Mesa pin/source diff, and staged Vulkan hash. The generated Playden
  `runtime-manifest.txt` will preserve those source diff hashes.
- Verification: both build scripts pass `bash -n`; `git diff --check` passes
  in both worktrees. A negative Playden preflight with a clean-looking
  temporary release manifest correctly rejected the dirty Orbis root before
  build steps. The current 02.16 manifest is also rejected at its nonempty
  Prosper-Win diff, as intended. These changes are local, uncommitted, and have
  not produced a new package.

## Update after local-commit authorization (2026-11-18)

- The user authorized local commits in Playden and Orbis-Win only; pushing and
  installing remain unauthorized. Playden commit `7f1020b3` contains the staged
  evidence gates, Mono/vkd3d staging, and tests. Orbis-Win commit `eb1e17a`
  pins the ABI-v4 pair and records/checks the exact Mesa and root provenance.
- Playden branch `sync/v0.12.0` is four commits ahead of its remote sync
  branch. Untracked `deps` and `scripts/network-deps/.deps` are dependency
  symlinks; preserve them. The Orbis-Win branch is clean for tracked files;
  untracked `prospero-win`, `sce_module`, and `sdk` are build symlinks.
- The Prosper-Win profile work remains dirty on separate branch
  `codex/profile-environment` at `a8f8850`. It contains the `[environment]`,
  `[audio] volume`, and `[steam] install_dir` parser/runtime changes and tests.
  The Playden build gate rejects that uncommitted tree, so a feature-enabled
  runtime stage/package cannot yet be built from the authorized clean pins.
  This separate Prosper-Win tree was not included in the user's local-commit
  authorization; ask before committing it.
- No new build, upload, or console install was performed. Focused checks pass:
  Playden kit-view 13/13; Prosper-Win profile/audio/start host tests; Orbis-Win
  shell syntax and `git diff --check`. The pinned Wine source tree is clean on
  `port/max-1086-1087` at
  `80169097f01c5eb752f97aa23ad34bfe5dc108fb`.
- The only Mesa checkout at pinned commit `0ec293c` has six local diagnostic
  edits. Its tracked files were left untouched. A clean detached worktree was
  created at
  `/home/zpeirce/junk/codex_scratch/mesa-ps4-pinned-0ec293c`, exact commit
  `0ec293c29f9735ee462eff0d9a9316481d741afa` (389 MiB, clean); pass this as
  `PW_RADV_MESA_SRC` for the ABI-v4 build.

## Console and upload update — 2026-11-18 19:53 AKST

This section supersedes earlier statements above that the profile tree was
dirty and the candidate package did not exist. The latest profile changes are
committed as Prosper-Win `8604f3b6`; Orbis-Win is at `db657ef7`, and Playden is
at `7f1020b3`. The Orbis-Win 02.17 and Playden 01.26 packages are built and
passed their recorded package parity and `pkg_validate` checks.

The PS4 MCP showed a system crash dialog for the installed Orbis title
`GLDS10043`, `Orbis Wine Driver Thread Parity`, with `CE-34878-0`. The dialog
was dismissed to the home screen, where Playden is installed. No Playden
package was installed. The fresh klog contains shell activity only, with no
Wine launch records for this Orbis run.

The user had directed that the completed Playden build be pushed to the
console. FTP to `10.168.2.52:2121` accepted `STOR` for
`/data/pkg/IV0000-PLDN10001_00-PLDN100010000000.pkg` but closed its data
connection on the first write (`Broken pipe`). A read-only `LIST` confirmed
that this target is now **0 bytes**. Do not install it. Existing Playden
packages `-0350` through `-0354` remain in `/data/pkg` at about 1.096 GB each.
The failed attempt does not establish why the data connection closed. The
PS4 Settings displayed 133.4 GB free, so the drive has room for this package.
A 25-byte uniquely named FTP probe was also accepted by `STOR`, then GoldHEN
returned `451 Local error in processing`; `RETR` returned 0 bytes and closed
the control connection. The subsequent `DELE` cleanup could not connect after
the FTP service stopped accepting connections. The probe may remain as a
zero-byte `/data/pkg/codex-ftp-probe.txt`. The separate title-log receiver on
188 is now listening on `0.0.0.0:9300`, writing under
`/home/zpeirce/junk/codex_scratch/ps5logd/build-ps4-logs/`.

GoldHEN Servers Settings initially showed FTP enabled. I toggled it off while
trying to restart the failed service, but the control did not turn back on and
port 2121 remained unavailable. The console is at the home screen with Playden
selected. Do not assume FTP is enabled now. Restoring it may require a console
restart, which has not been performed.

Next: diagnose the GoldHEN FTP data-channel failure and recover the standard
Playden target before attempting another upload. Do not remove the existing
`-0350` through `-0354` files without the user's direction. For the next
Orbis launch, the PS4 logger must be kept pointed at `10.168.2.188:9300` and
its receiver left running before launch.

## Playden 01.26 / Half-Life 2 run — 2026-11-18 20:36 AKST

The user authorized installing Playden 01.26 on the PS4. GoldHEN package
installation completed successfully after FTP upload and read-back verification;
the installed title is PLDN10001. Playden reconnected to the saved Steam session
and populated the library.

Half-Life 2 was launched from profile `playden-steam-220` (`steam-220`, AppID
220). The runtime log identifies Orbis-Win marker
`GLDS10043-02.17-profile-parity`, native WoW64, and the expected profile feature
marker `environment,audio-volume,steam-library-path`. Wine loaded `hl2.exe`,
`steam_api.dll`, the Source engine/client/server/GameUI modules, initialized
Vulkan, and logged `PW_VK_PRESENT cursor hook reached`. Remote Play screenshot
`/home/zpeirce/junk/codex_scratch/playden-0126-hl2-current.png` shows the actual
Half-Life 2 main menu, so video rendering and launch succeeded. This is the
first Playden 01.26 title qualification; it is not yet a full gameplay/input,
audio, or clean-exit pass. A remote Cross press did not visibly change the menu,
so controller input remains inconclusive.

- Runtime log: `/home/zpeirce/junk/codex_scratch/build-ps4-logs/ps5log-20261118-203451.log`
- Current runtime marker: `GLDS10043-02.17-profile-parity`
- Package SHA-256 verified on the PS4: `5b32558b72f7f1d316c284beb56fa6481ef06ffc74d490d92115f37e40ded135`
- Package: `/home/zpeirce/junk/codex_scratch/runtime-kit-parity-build-20261118/playden-profile-parity-0126/package/IV0000-PLDN10001_00-PLDN100010000000.pkg`
- Run connected at 20:36:22 AKST. Do not read `frames_put=0` as a Vulkan failure
  here: the screenshot proves the Vulkan path rendered the menu; that counter is
  not the game's Vulkan-present counter. Keep `PW_VK_PRESENT`/actual output
  evidence as the rendering discriminator.
- The active screenshot has the menu displayed. This run is not crash-to-PS4-
shell; no guest exception or exit was logged in the captured interval.

The remaining console qualification is unchanged: finish a brief gameplay test
and verify pad/audio/clean return to Playden for HL2, then FNV/Portal/Mafia;
test profile environment override, reduced audio volume and Steam install-dir
lookup one at a time; run a 32-bit hosted-driver title and validate 1076 markers;
run a .NET title for Mono. vkd3d-proton remains staged but unclaimed until a
D3D12 game renders. The 01.26 package passed parity and `pkg_validate`; no
release or GitHub push was performed.

### Follow-up checks on the 01.26 run

- The Playden library startup record says `entries=1 games=1` under
  `/data/playden/games/steam-220/library`, so HL2 is the only currently
  available regression title in this library.
- The staged 01.26 view claims 17 capabilities. I rechecked the actual package
  stage: both 32-bit and 64-bit `ntdll.dll` and `ntoskrnl.exe` contain patch
  1076's `hosted driver thread %04lx: no thread attach` marker and the
  `C:\windows\syswow64\drivers\` path. HL2 runs as a 32-bit guest under
  `wow64native.dll`, but the log has no hosted-driver attach event; the specific
  1076 runtime path therefore remains unqualified on hardware.
- Two Remote Play Cross/D-pad presses did not visibly move the HL2 menu. The PS
  button successfully returned to the PS4 home screen, and Cross resumed the
  suspended HL2 session. This proves the remote control link works at the shell,
  but gamepad input and clean in-game exit are still unverified. The game is
  currently resumed at its main menu; no guest exit was observed.
- The log confirms the audio sink opened (`volume=100`) and audio callbacks ran,
  but the default-volume run does not qualify reduced per-title volume. The
  `profile_features` line proves the environment/audio/Steam path code is in
  this runtime; nondefault environment/volume and `[steam] install_dir` behavior
  still need isolated profile tests.

### End of this console session

To bound the verbose logger output, I used the PS button, selected Playden on the
PS4 home screen, then Options > Close Application and confirmed. The PS4 is now
at the home screen with Playden selected and ready to start. The Wine logger
connection ended at 20:48:43 AKST; the session did not emit a Wine guest-exit
record, so this does **not** qualify a clean in-game exit/return to Playden.
The captured log is 49,574,966 bytes. The `ps5logd` receiver remains listening
on port 9300 for a later test. Final home screenshot:
`/home/zpeirce/junk/codex_scratch/playden-0126-after-close.png`.

## Playden 01.26 / Mafia launch — 2026-11-18 21:19 AKST

Mafia (Steam AppID 40990) was launched from Playden after the runtime kit update.
The runtime resolved `install_dir=Mafia` to `/data/PLDN10001/games/40990`, used
profile `playden-steam-40990`, and reported marker
`GLDS10043-02.17-profile-parity`. It loaded DXVK/Vulkan and reached a rendered
Mafia intro, then the game's profile-name dialog. Screenshot evidence:

- `/home/zpeirce/junk/codex_scratch/playden-0126-mafia-running.png`
- `/home/zpeirce/junk/codex_scratch/playden-0126-mafia-current.png`
- `/home/zpeirce/junk/codex_scratch/playden-0126-mafia-profile-ok.png`

The Wine log recorded `PW_VK_PRESENT cursor hook reached`, audio sink status
`ok` at volume 100, and continuing nonzero audio callbacks. Repeated heartbeat
records remained alive through the profile dialog. This is a successful launch
and rendered-video/audio-path check, not a playable-game, controller-input,
Mono, hosted-driver, or clean guest-exit qualification. Cross, D-pad, and
Chiaki's keyboard mapping did not advance the profile dialog in this run; input
therefore remains inconclusive (the Remote Play keyboard mapping itself is a
limitation of this control path).

The PS4 home screen was used to close the parent Playden application. The
logger disconnected at 21:19:01 AKST without a Wine guest-exit record, so do not
count that as a clean return-to-Playden test. Final screenshot:
`/home/zpeirce/junk/codex_scratch/playden-0126-mafia-close-confirm.png`.
The combined log, including both HL2 and Mafia sessions, is
`/home/zpeirce/junk/codex_scratch/build-ps4-logs/ps5log-20261118-203451.log`
(66,605,466 bytes). The PS4 is at the home screen with Playden selected.

### Mafia profile-volume A/B attempt and restoration — 2026-11-18 22:22 AKST

In Playden 01.26's Mafia > More > Game settings, set Volume to 50%. The UI
showed a custom profile with one change (`Volume 50%`) and said it was saved for
next launch. Leaving the editor then displayed: “Mafia: not done — The settings
could not be applied: Requested profile is unsupported or has mismatched
identities.” Mafia was not launched with the override. The setting was reset
through “Reset all to preset”; the resulting screen confirms “Every setting
follows Playden default” and “Nothing to reset.”

This is a failed settings-publication attempt, not evidence that the runtime's
volume control works. In the Playden source, the exact rejection is in
`src/runtime/settings_publication_journal.cpp` `RequestValid()`: after parsing
the profile, its app ID, runtime bundle ID, or `prefix="game"` disagrees with
the request/install identity. The on-screen error alone does not identify which
field differs. The app's detailed `runtime.log` should be collected next to
capture the `settings update request` reason before retrying. The temporary 50%
override is cleared in the UI.

Screenshots: `playden-0126-mafia-volume-50.png`,
`playden-0126-mafia-after-error.png`, and
`playden-0126-mafia-reset-result.png` under
`/home/zpeirce/junk/codex_scratch/`. The PS4 is currently in Playden on the
Mafia game card; Playden reports Mafia setup needs repair because game setup
files changed. The Wine logger receiver remains active on port 9300.

### Audio-volume profile verifier fix / candidate 01.27 — 2026-11-18

The Playden runtime log at
`/home/zpeirce/junk/codex_scratch/playden-runtime-0126.log` resolves the prior
generic UI error: the settings update was rejected with
`publication-request-invalid`, specifically “Requested profile is unsupported
or has mismatched identities.” The runtime profile emitted by Orbis-Win 02.17
contains `[audio] volume=50`; Playden's native profile verifier was still pinned
to Prosper-Win `80b7cad`, which does not parse `[audio]`. The runtime parser in
Prosper-Win `8604f3b6` supports it.

Rather than replacing Playden's divergent Prosper-Win pin wholesale, an
isolated worktree was created at
`/home/zpeirce/junk/codex_scratch/playden-profile-parser-fix-20261118` on
`codex/kit-volume-profile-parser`, based on Playden `7f1020b3`. It ports only
the audio profile parser support into the Playden-pinned parser, with focused
tests for the default and 0/50/100 values, and rejection of >100, text,
duplicates, and unknown keys. The parser tests passed. Local commits (not
pushed): Prosper-Win `cc328b9eab86fad393351798de1e69422dbbdcd0`
(`profile: accept audio volume in verifier`) and Playden
`4fb2cf9ec4a3cb021fe6d27ab5b930e0715bd2d6`
(`profile: pin Playden verifier with audio volume support`).

The candidate Playden 01.27 app/runtime stage was assembled at
`/home/zpeirce/junk/codex_scratch/runtime-kit-parity-build-20261118/playden-profile-parser-0127/pkg-stage`
using the exact 02.17 Orbis-Win stage/runtime. Required parity check against
01.26 exited 0: 0 hard failures and 0 baseline payloads absent. It printed
warnings because the 02.17 release does not retain matching ntdll/wineserver
ELFs, so those export/signal-mask checks were skipped. The 01.27 installer is
built at
`/home/zpeirce/junk/codex_scratch/runtime-kit-parity-build-20261118/playden-profile-parser-0127/package/IV0000-PLDN10001_00-PLDN100010000000.pkg`
(the package output directory is on `/home/zpeirce/ps4-package-scratch/` to
avoid the full `~/junk` volume). SHA-256:
`03737c770be12b602b7c12c506aca4c12aeefd4ae2eaa56f3b96ccaf6a3dc105`.
`PkgTool.Core pkg_validate` passed every hash/signature check. The incomplete
6 MB eboot-only package from the first attempt was removed; the completed
candidate stage, 01.26 baseline package/stage, and installed 01.26 were
preserved.

The 01.27 candidate has not been installed or pushed. The console remains on
Playden 01.26; Mafia's volume override remains reset. Next, install the exact
01.27 installer above and repeat the Mafia volume=50 profile update; the
specific expected result is successful publication, followed by a launch that
logs the requested audio volume. Keep the test isolated to this one setting.

## Continuation audit — 2026-11-18 after console reboot

### Current console and Wine evidence

- A fresh PS4 MCP snapshot after reboot is saved at
  `/home/zpeirce/junk/codex_scratch/ps4-current-after-reboot.png`. It shows
  Playden on Mafia's Game settings panel. This is the current console state;
  the 01.26 install is still present.
- The active raw TCP receiver is
  `/home/zpeirce/junk/codex_scratch/ps5logd-runner/ps5logd.py` on port 9300.
  Its log file is
  `/home/zpeirce/junk/codex_scratch/build-ps4-logs/ps5log-20261118-203451.log`.
  It contains no new Wine session after the 21:19 Mafia run; later appended
  lines are HTTP text from an unsuccessful curl probe, not console logs.
- The 21:13 Mafia launch is affirmative Wine evidence: marker
  `GLDS10043-02.17-profile-parity`, `steam view install_dir=Mafia` resolved
  successfully, Wine started, loaded the game and its native DLLs, DXVK 2.6.2
  initialized Vulkan, and audio callbacks continued. Earlier screenshots show
  the rendered intro and profile dialog. The game did not produce a clean
  guest-exit record; Playden was closed from the PS4 home screen. Do not call
  this a successful clean-exit or full gameplay/input qualification.

### 01.27 package gate recheck

- Exact installer:
  `/home/zpeirce/ps4-package-scratch/playden-profile-parser-0127-package/IV0000-PLDN10001_00-PLDN100010000000.pkg`
- SHA-256 rechecked as
  `03737c770be12b602b7c12c506aca4c12aeefd4ae2eaa56f3b96ccaf6a3dc105`.
- `scripts/check-ps4-package-parity.py` rerun against the installed 01.26
  stage and 01.27 candidate, with the Prosper source at the recorded 8604
  commit. Exit status 0; 0 FAIL; no baseline payload absent. The report is
  `parity-current.txt` beside the installer.
- `PkgTool.Core pkg_validate` rerun against this exact installer; every size,
  digest, and signature check is `[OK]`. Report is `pkg_validate-current.txt`
  beside the installer.
- The parity report's WARNs are specifically the skipped ntdll/wineserver
  export and signal-mask checks: the build kept the PRXs but did not retain
  their matching unstripped `*.shared.elf` files or descriptor source. This is
  an evidence gap, not a hard-gate failure; do not report those checks as
  passing. The 32-bit `kernelbase.dll` has 25 `PW_EXCEPTION32` strings, and
  the matching host Wine source confirms the no-debugger path calls
  `log_exception_context32`.

### Public Prosper-Win source check

- The test-only `cc328b9` parser port is a private local commit and its lock
  correctly has `published=false`; `tools/prospero_source.py --release`
  rejects it. This blocks a release package built from that exact pin.
- The fetched public `leolobato/prospero-win` `origin/main` commit is
  `c3847a3557000b48127c9c563f7a82dee29a7bdc`. It contains the `[audio]
  volume` parser/runtime implementation (`c52eff7`) and also contains the
  environment and Steam install-directory support. Its host profile and audio
  mix tests passed, as did Playden's 13 kit-evidence tests while temporarily
  checked against that public source; the nested source checkout was restored
  to `cc328b9` afterward.
- `c384` is public but brings 43 files / 3,668 added lines relative to the
  narrow local `cc328` parser patch, including microphone/capture work. Do not
  silently replace the narrow test pin with this broader source delta; evaluate
  it as an explicit dependency update, with its own build and hardware test.

### Remaining phase qualification

- Phase 0 user-facing behavior is incomplete: frame-generation selection,
  rumble, and visible FPS overlay have not yet been observed working on this
  package.
- Phase 1 is complete locally: `tests/test_ps4_kit_view.py` passes 13/13 and
  claims are gated on staged evidence.
- Phase 2 is incomplete on hardware: both 1076 PE markers are staged, but no
  hosted-driver attach event was observed. There is no available GameMaker or
  NW.js title in the currently surveyed installed Steam set.
- Rechecked Steam manifests on the PC: installed apps include Cyberpunk 2077
  (75.84 GiB) and Starfield (128.46 GiB), both potential D3D12 qualification
  candidates if copied/installed on the PS4. The local `3DMark` directory is
  empty. The surveyed app set has no installed GameMaker, NW.js, or ordinary
  Windows .NET qualification title; Unity games' bundled Mono does not qualify
  Wine Mono.
- Phases 3, 4, and 6 have compiled source and profile parser support. A custom
  environment override, reduced volume, and a title's own Steam install-path
  lookup still need isolated hardware tests. The 01.26 volume test was rejected
  by the old verifier and reset; it did not launch with volume 50.
- Phase 5 stages Wine Mono and initializes the template registry, but no .NET
  title is available in the surveyed local Steam install for qualification.
- Phase 7 remains intentionally unclaimed. A D3D12 title must render on the
  console before enabling `vkd3d-proton`.
- Playden `sync/v0.12.0` still has four local commits ahead of its remote; no
  push has been authorized. No 01.27 installation has occurred. User approval
  is pending for installing the exact 01.27 installer above.

## Console reconnected after the reboot (2026-11-18 22:57-22:58 local)

- The active screen is Playden on the Mafia card. Playden 01.26 remains installed;
  the prior user-approved 01.26 test install is already satisfied. Do not treat
  02.15 as the latest runtime: it was the comparison baseline. The latest
  recorded runtime run is Orbis-Win `GLDS10043-02.17-profile-parity`.
- A read-only Chiaki snapshot succeeded at
  `/home/zpeirce/junk/codex_scratch/console-after-reboot-20261118.png` (the CLI
  returned an error only after capture, because `ffmpeg` is missing for its
  luma calculation). The PS4 MCP's stale app PID did not prevent direct Chiaki
  keyboard-to-pad control; its local `windowactivate` fix made Circle/Cross
  presses reach the console.
- Backing out of Mafia's settings exposed a Playden message:
  `Before state is not a verified committed complete publication.` The card
  reports `Setup stopped early` / `Interrupted` and offers `Finish setup`.
  Dismissing the message returned to the Mafia card and showed a toast that
  settings were saved with one changed setting. No game was launched, no
  publication repair was attempted, and there is no new Wine run in the log.
- Playden source locates this refusal in
  `src/runtime/settings_publication.cpp` around the prior publication receipt,
  metadata, descriptor, and committed-state verification. It means the
  existing Mafia publication could not be proven intact before a settings
  update. It is a Playden title-state problem; current evidence does not tie it
  to Orbis-Win 02.17. Do not click `Finish setup` or change Mafia settings until
  its interrupted publication state is understood.
- The current receiver log remains
  `/home/zpeirce/junk/codex_scratch/build-ps4-logs/ps5log-20261118-203451.log`;
  it contains the prior 21:19 Mafia run but no post-reboot Wine session. The
  exact Playden package currently on the console is 01.26; 01.27 remains only a
  locally validated candidate and still requires separate install approval.

## HL2 run and profile test (2026-11-18, after reboot)

- On Playden 01.26, Half-Life 2 showed `Needs repair` / `Play Repairs first`.
  Selecting it entered `Preparing` and then launched the game with Orbis-Win
  `GLDS10043-02.17-profile-parity`. The runtime resolved Steam app 220 and
  launched `hl2.exe` with `-game hl2 -steam -w 1920 -h 1080 -novid +exec
  360controller.cfg +joystick 1`. It took about three minutes to reach the
  rendered HL2 main menu; audio callbacks later became audible. The live screen
  proves this was a slow startup, not a permanent spinner.
- The run reported `cpu=native backend=wow64native.dll`, DXVK initialized, and
  no Wine exception. Early `PW_WINE64` lines had `frames_put=0` while the PS4
  showed its loading spinner; the menu eventually appeared. Do not treat
  `frames_put=0` by itself as proof of a render failure for this run.
- D-pad presses through Chiaki navigated the Playden UI. They did not visibly
  move HL2's main-menu selection; controller gameplay is not qualified. The PS
  button opened Quick Menu and the close flow returned to PS4 home. The Wine
  log disconnected at 23:10:07 without a guest-exit/session-end record; do not
  describe it as a clean guest exit.
- FPS overlay was Off in HL2's profile. Selecting On changed the local profile,
  but leaving Game Settings returned `Half-Life 2: not done` with the same
  `Before state is not a verified committed complete publication` error seen
  for Mafia. Thus overlay-On was **not** applied to the title. Reset all to
  preset restored HL2's profile to Playden default; the card now confirms
  Game settings = Playden default. Do not count the FPS test as passed.
- HL2 still shows `Setup stopped early` / `Interrupted` and a `Finish setup`
  action after the run. Selecting Finish setup did not clear that state. The
  same publication guard blocks both Mafia and HL2 settings changes; investigate
  this Playden title-publication state before further profile tests. Do not
  change settings or install a newer package as a guessed fix.
- Exact evidence: raw log
  `/home/zpeirce/junk/codex_scratch/build-ps4-logs/ps5log-20261118-203451.log`
  (HL2 session begins at line 703600; ends with connection disconnect at
  23:10:07); screenshots under
  `/home/zpeirce/junk/codex_scratch/console-hl2-*.png`. The console is left
  in Playden on the HL2 card, with its profile reset to Playden default.

### Read-only console filesystem probe

- GoldHEN FTP on `10.168.2.52:2121` is reachable with anonymous read access.
  `/data/PLDN10001` is mode `0711` and `/data/playden` is mode `0700`; their
  directory listings are denied. The public game tree `/data/PLDN10001/games/220`
  is readable, but the settings receipt and profile metadata live under the
  private `/data/playden/...` tree. No files were written or permissions changed.
- Therefore the current FTP path cannot expose `publication-state.v1` or
  `.playden-publication`. The next diagnosis needs a read-only in-app diagnostic
  or an explicitly approved Playden recovery path; do not weaken permissions or
  modify private publication files through FTP.

## Console reconnect after the later reboot — 2026-11-19

- The user reported that the PS4 was back online. A fresh Remote Play snapshot
  is saved at
  `/home/zpeirce/junk/codex_scratch/console-reboot-check-20261119/current.jpg`.
  It shows Playden on the Half-Life 2 card with `Setup stopped early` and
  `Setup: Interrupted`.
- The recorded Orbis-Win runtime on the console is still
  `GLDS10043-02.17-profile-parity`. 02.15 was only the comparison baseline used
  to validate the 02.17 package.
- GoldHEN FTP at `10.168.2.52:2121` now refuses connections after reboot, so
  the private Playden publication receipt and metadata remain unreadable by
  that route. No setup repair, profile write, FTP write, or install was
  attempted in this checkpoint. Playden 01.27 remains an uninstalled local
  candidate; the only explicit install authorization was for 01.26.
- The previous `ps5logd` capture ends with the 2026-11-18 HL2 session and
  Remote Play disconnect; it contains no post-reboot Wine run. Restart the
  klog reader before any new title launch, and use a read-only in-app/export
  path to diagnose the shared publication-state refusal.

## Hosted-driver profile activation follow-up — 2026-11-19

- Source inspection found why staged patch 1076 cannot yet be enabled per
  title: `ntdll` and `ntoskrnl` require `WINE_NTOSKRNL_HOSTED` in the title
  environment, but Playden's profile allowlist rejected it. A narrow Playden
  change now accepts only value `1`, requires the `wine-driver-threads` kit
  capability, includes the name in the settings hint, and prevents community
  recipes from setting it. Value `0` is rejected because the runtime enables
  the feature for any nonempty value.
- The five-file change is in
  `/home/zpeirce/junk/playden-upstream-sync-1118/trial` on `sync/v0.12.0`.
  `git diff --check` and Clang C++17 syntax checks passed for both changed
  runtime sources and both changed test sources. Full linked unit tests and a
  package build have not run: this host lacks the PS5 SDK and the direct
  Homebrew UI host build is guarded to macOS. No new package was built,
  installed, or pushed.
- The console is back online, but its last evidence remains the interrupted
  HL2 setup on Playden 01.26. The newest captured Wine launch remains the
  02.17 runtime launch from 2026-11-18; 02.15 was only the comparison baseline.
  There is no new post-reboot Wine run. This session exposes no PS4 MCP control
  tools, so no console input was sent.
- Do not install or push a candidate until the Playden change is fully built
  and checked. The earlier install authorization covered 01.26, which is
  already installed; a new candidate needs separate authorization.
- Local checkpoint `1145cfa3` contains only the five-file profile-setting
  change; `sync/v0.12.0` is now five commits ahead of its remote and has not
  been pushed. In an isolated host build, `community_rules` passed. The
  unmodified `game_settings` test stops first at an existing catalogue check:
  `graphics` has `also_called = "[application] graphics"`, violating its
  assertion that aliases contain no profile keys. The temporary diagnostic
  skip then segfaulted, so the new profile assertions are not yet confirmed by
  a complete test run. The tracked CMake file also declares
  `pe_architecture_test` twice; both issues were only bypassed in temporary
  build inputs and the project tree was restored.

## Recheck of hosted-driver capability evidence — 2026-11-19

- A content-based `audit-patch-presence.py` run against the current Prosper-Win
  Wine Unix and host source trees confirms the archived Orbis-Win hosted-driver
  implementation is present in source. The report is
  `/home/zpeirce/junk/codex_scratch/kit-parity-audits-20261119/orbis-archive-vs-profile-source.txt`.
- Direct inspection of the exact 02.17 Orbis-Win package stage found the
  `hosted driver thread %04lx: no thread attach` marker in both 32-bit and
  64-bit `ntdll.dll`, and `hosted driver %s` in both architecture-specific
  `ntoskrnl.exe` files. The same four binaries in Playden 01.26 and 01.27 have
  byte-identical SHA-256 hashes to 02.17. Both Playden kit-view manifests
  claim `wine-driver-threads`.
- This supersedes the earlier stage-specific note that the selected runtime
  lacked 1076: that observation applied to the older profile-version stage,
  not the current 02.17/01.26 package. Package contents and claim are now
  evidenced; hardware qualification is still pending a title that actually
  exercises hosted-driver startup/thread attach. HL2 has not produced that
  event, and no suitable GameMaker or NW.js test title is currently available.

## Playden 01.28 package candidate — 2026-11-19

- Built a Playden 01.28 candidate on Slackian using the exact Orbis-Win 02.17
  source/stage already selected for this parity effort. Orbis-Win was not rebuilt.
  Package:
  `/home/zpeirce/codex_scratch/playden-hosted-driver-0128/package/IV0000-PLDN10001_00-PLDN100010000000.pkg`
  SHA-256: `4480be6b2c1695dc078fb8e32c9bb2d95bd8a65a7cf169439adb324f428f4270`
- Package SFO reports title ID `PLDN10001`, version `01.28`, content ID
  `IV0000-PLDN10001_00-PLDN100010000000`. The prior `pkg_validate` run passed
  all 28 checks. Candidate-to-01.26 package parity exited 0 with zero missing
  baseline payloads and zero hard failures. It warned that matching unstripped
  `ntdll.shared.elf` and `wineserver.shared.elf` were unavailable, so export
  and signal-mask checks were skipped.
- Extracted package files match the staged build byte-for-byte for
  `runtime-manifest.txt`, `runtime-template/playden-kit-view.json`, `dev.conf`,
  `sce_module/relocated_wine_app.prx`, and `sce_module/playden_ui.prx`.
  `playden_ui.prx` contains `WINE_NTOSKRNL_HOSTED`; the kit view claims
  `wine-driver-threads`, `wine-mono`, and `settings-profile-environment`.
  vkd3d-proton DLLs are staged but the `vkd3d-proton` capability remains
  unclaimed pending hardware qualification.
- `dev.conf` points to `10.168.2.188:9300`. The package is not installed or
  uploaded. The earlier install authorization was specifically for 01.26,
  which is already installed; 01.28 requires its own explicit install approval.
- Host-test limitations remain: `community_rules_test` passed, but the full
  `game_settings_test` run is blocked by the pre-existing `graphics` alias
  invariant and a temporary diagnostic skip segfaulted. Syntax checks passed.
  See the prior sections for the exact test environment and local commit
  `1145cfa3`; no push was made.
- Console is back online, but no post-reboot Wine session has been captured.
  The current tool catalog still exposes no callable PS4 MCP controls. The
  last recorded console runtime is Orbis-Win 02.17 through installed Playden
  01.26; 02.15 was only the comparison baseline. Do not claim 01.28 hardware
  qualification until it is explicitly installed and exercised on console.

## Console connectivity recheck — 2026-11-19 00:30 local

- The PS4 is reachable again. A fresh Chiaki snapshot is at
  `/home/zpeirce/junk/codex_scratch/continuation-20261119/console.jpg`; it
  shows Playden on the Half-Life 2 card with `Setup stopped early` and
  `Setup: Interrupted`.
- GoldHEN FTP on `10.168.2.52:2121` now accepts connections. `/data/pkg` is
  listable and contains the existing versioned packages (`-0350` through
  `-0354`) and the plain-name Playden package. `/user/app/PLDN10001/app.json`
  points at `/user/data/pkg/IV0000-PLDN10001_00-PLDN100010000000.pkg`; FTP
  reports 1,547,501,568 bytes. This size alone does not identify the installed
  version. The candidate 01.28 package has the same byte size, so do not infer
  it is installed from size or path.
- FTP still cannot list `/data/playden` or the private `/data/PLDN10001` root;
  attempts return 550 / not-a-directory. It can list the public game folders
  under `/data/PLDN10001/games`. No console files were written, uploaded,
  removed, or permission-changed in this check.
- This session has no callable PS4 MCP tool, but Chiaki snapshot and the
  installed `ps5mcp` CLI work for read-only observation. The current app state
  does not provide a post-reboot Wine launch. The log collector listens on the
  host's port 9300; no new console runtime log has been captured.
- 01.28 remains a candidate only. The earlier install authorization covered
  01.26; do not upload or install 01.28 without specific authorization. The
  profile publication interruption also means a setting write is not currently
  a valid hardware test.
- The host `ps5logd.py` receiver is live on port 9300, but
  `receiver.out` was last modified 2026-11-18 23:10 local and ends with the
  console disconnect at 23:10:07. It is listening for the next app connection;
  it has not captured a post-reboot run.
- The extracted 01.28 kit view has exactly 17 capability claims:
  `display-fps-counter`, `display-framegen`, `environment-file-budgets`,
  `input-keyboard-stick`, `input-rumble`, `input-touchpad`, `opengl-zink`,
  `profile-version-2`, `settings-audio-volume`, `settings-dxvk-config`,
  `settings-profile-environment`, `steam-library-path`, `title-any-id`,
  `wine-driver-threads`, `wine-large-address-aware`, `wine-mono`, and
  `wine-shared-sections`. The 18th target, `vkd3d-proton`, is intentionally
  absent from claims until a D3D12 title renders.
- Rechecked the extracted 01.28 runtime itself (not just the older 02.17
  source stage): both x86 and x86_64 `ntdll.dll` contain
  `hosted driver thread %04lx: no thread attach`, and both matching
  `ntoskrnl.exe` files contain `hosted driver %s`. The package also contains
  these binaries in the template bottle. This proves the provider and profile
  opt-in are co-packaged; it does not prove a title exercises the path.

## Readable Playden log evidence for the interrupted settings setup — 2026-11-19

- Retrieved the readable `/data/PLDN10001-network.log` to
  `/home/zpeirce/junk/codex_scratch/continuation-20261119/PLDN10001-network.log`.
  It is historical (last entries are preview-loop output, no post-reboot launch).
- The last settings transaction in that log is app `0xF0000007` and fails with
  `Before state is not a verified committed complete publication.` The source
  guard is `settings_publication.cpp`'s preflight of a committed receipt, the
  publication metadata, the `game-launch.json` descriptor, and the locked
  publication contents. FTP cannot expose the private receipt/descriptor, so
  this line does not identify which of those checks failed.
- The same historical log records multiple game settings failures because
  `user.reg` was mode 438 decimal (`0666`), including Steam IDs 250900, 220,
  400, 367520, and 632360. The checked source's `RepairBottle()` is designed
  to restore registry hives to `0600`; it is invoked by explicit Prepare or
  Maintenance Repair flows. This mode issue is a separate observed failure and
  is not proven to be the cause of the `0xF0000007` publication refusal.
- No private file was read or changed, and no repair was triggered. Diagnose
  the publication refusal through Playden's own diagnostic/repair flow before
  attempting profile capability tests; do not infer a safe FTP chmod target.

## Console reconnect after reboot — 2026-11-19 01:18 AKDT

- Chiaki Remote Play is connected after the console reboot. A fresh screenshot
  is `/home/zpeirce/junk/codex_scratch/continuation-20261119/console-after-reboot.png`.
  Playden remains open on the Half-Life 2 details page with `Setup stopped early`
  and `Setup: Interrupted`; there is still no post-reboot Wine launch.
- This session has no callable PS4 MCP controls. Return and Right sent to the
  focused Chiaki window did not change the console screen, so no setup repair,
  package install, or private-data write was performed.
- The only explicit package-install authorization was for 01.26, already
  installed per the prior hardware run. 01.28 remains uninstalled; its same-size
  package cannot be distinguished from the installed package by FTP size.

## Controlled Finish setup retry — 2026-11-19 01:20 AKDT

- Although PS4 MCP tools are not exposed in the tool catalog, the installed
  Linux adapter is callable through
  `/home/zpeirce/codex_scratch/ps4-mcp-venv/bin/python` using
  `ps5mcp.linux_chiaki.LinuxChiakiClient`. This uses the adapter's discovered
  Xwayland authorization; direct `xdotool` without that environment did not
  send input.
- Pressed Cross on the highlighted Half-Life 2 `Finish setup` action. The UI
  advanced to `Step 5 of 6 — Applying settings`, then returned to `Setup
  stopped early` / `Setup: Interrupted`. Screenshot:
  `/home/zpeirce/junk/codex_scratch/continuation-20261119/console-after-finish-setup.jpg`.
- Retrieved the current network log after the attempt to
  `/home/zpeirce/junk/codex_scratch/continuation-20261119/PLDN10001-network-after-finish-setup.log`.
  It contains no new settings-transaction error after the button press; the
  recorded publication refusal remains startup app `0xF0000007` at line 327927.
  Startup also reports `keyboard_stick: unknown movement stick` for multiple
  generated profiles. This matches the Playden source bug fixed in local
  commit `4965c770`, but does not establish that it is the cause of the
  separate publication refusal.
- The retry did not write, repair, or reinstall a bottle/package. The 01.28
  candidate remains uninstalled. The 01.29 candidate below now includes the
  `4965c770` fix; it has not been uploaded or installed.

## Playden 01.29 profile-fallback candidate — 2026-11-19

- The 01.28 package was linked at 01:21 AKST, while its
  `libplayden_ps4_game_settings.a` was last built at 23:59, before local commit
  `4965c770` (00:59). The source fix therefore was not in the 01.28 PRX. Rebuilt
  only `playden_ps4_game_settings`, then relinked `playden_ui.prx` against the
  same 01.28 package stage and runtime payload. This is a controlled one-code
  change; the original Slackian source checkout was not edited.
- New UI PRX SHA-256:
  `44abad05a6a32bf88d5f1f55a4951b02cb6fd5d3cae42e15d1d46585d05f2ce9`.
  PRX BSS coverage passed. The candidate stage differs from 01.28 in the UI PRX,
  `runtime-manifest.txt`, `sce_sys/param.sfo`, and package-build log only; all
  other staged files are hard links to the unchanged 01.28 stage.
- Installer:
  `/home/zpeirce/codex_scratch/playden-profile-fallback-0129/package/IV0000-PLDN10001_00-PLDN100010000000.pkg`
  on Slackian; SHA-256
  `6f89ba5a875d863b488f01ad03abe34e8736f8cef632e99082abb9676876e611`.
  Its staged version and provenance record are `01.29` and
  `profile_fallback_fix_commit=4965c7703b912568890f041c2254ff0d5fc29200`.
- Package gates: parity against the installed 01.26 stage exited 0 with no hard
  failures and zero baseline payload omissions. It warns that unstripped
  `ntdll` and `wineserver` ELFs are absent, so those matching-export and
  signal-mask checks are skipped; these modules were not changed from 01.28.
  `PkgTool.Core pkg_validate` passed all 28 checks. Extracting the package
  produced 6,505 payload files, all byte-identical to the stage. The SFO and
  icon are package-header metadata and `keystone` is generated during packaging,
  so they are outside that payload comparison; the SFO stage is version 01.29
  and its digest passed package validation. Compared with 01.28, only the SFO's
  `APP_VER` and `VERSION` fields changed; the other eight metadata fields match.
  Logs are in the candidate directory (`pkg-stage/pkg-build.log`,
  `package/*.pkg.validate.log`, and `pkg-extract.log`); parity output is
  `/home/zpeirce/ps4-package-scratch/playden-profile-fallback-0129/parity-v0126.txt`.
- The fix handles preset-only `movement_stick` values when no catalogue row is
  available. It does not address the separate startup failure:
  `Before state is not a verified committed complete publication.` The console
  remains on the HL2 `Setup stopped early` page after reboot, with no Wine run.
  The current screen capture is
  `/home/zpeirce/junk/codex_scratch/continuation-20261119/console-current-after-0129-build.jpg`.
- The user authorized installation of 01.26 only; 01.29 has not been uploaded
  to the console or installed. Do not treat package validation as console
  qualification. The next hardware comparison requires explicit approval to
  install this new package, and the publication-state refusal remains a
  separate blocker for settings qualification.

## Playden 01.30 publication-diagnostics candidate — 2026-11-19

- The console still shows HL2 `Setup stopped early` / `Setup: Interrupted`;
  fresh screenshot:
  `/home/zpeirce/junk/codex_scratch/continuation-20261119/console-current-after-0130-build.jpg`.
  No game or Wine process was launched.
- Added read-only detail to the publication preflight failure in Playden commit
  `938f31921ff15370e8197998f2b685a1febb4e85` on local branch `sync/v0.12.0`.
  It distinguishes invalid/uncommitted receipt, missing publication metadata,
  descriptor parse failure or app-id mismatch, and locked-content verification
  failure. The underlying checks and write behavior are unchanged. This commit
  is local and has not been pushed.
- Rebuilt the Playden UI PRX with both commit `4965c770` (preset-only
  `movement_stick` fallback) and `938f3192` (publication diagnostics).
  PRX BSS coverage passed. The 01.30 stage is based on 01.29; only
  `playden_ui.prx`, `runtime-manifest.txt`, and `sce_sys/param.sfo` differ.
  SFO comparison confirms only `APP_VER` and `VERSION` changed from 01.29.
- Installer on Slackian:
  `/home/zpeirce/codex_scratch/playden-profile-diagnostics-0130/package/IV0000-PLDN10001_00-PLDN100010000000.pkg`
  SHA-256 `19fa53425f1241044990b59d1cdd4dd294eeefcbb579cb5760ce3b76dfee1134`.
  `PkgTool.Core pkg_validate` passed all 28 checks. Extracted payload comparison
  passed: 6,505 stage files and 6,505 extracted files, with zero missing,
  extra, or differing files. Package parity against installed 01.26 exited 0
  with zero hard failures and zero baseline omissions; it retains the warnings
  that the matching unstripped `ntdll` and `wineserver` ELFs are missing.
- Re-read the 01.30 kit view: it has 17 claims, including Mono, hosted-driver,
  profile environment, audio volume, and Steam library path. vkd3d-proton is
  staged but remains unclaimed. All four vkd3d DLLs are in the x86/x64 kit
  paths with PE machines 0x14c/0x8664; Mono 11.3.0 is staged in the template.
  These are package-content checks, not .NET or D3D12 hardware qualification.
- This package has not been uploaded or installed. The only explicit
  installation authorization still covers 01.26. For the next diagnostic run,
  01.30 supersedes 01.29 because it contains the same fallback fix plus the
  precise publication failure reason. Do not count this as console
  qualification until that package is approved, installed, and observed.

## Remaining console qualification order

1. After explicit authorization, install 01.30 and finish the HL2 setup once.
   Capture the new publication diagnostic and `/data/PLDN10001-network.log`.
   Do not run profile-setting tests until the publication preflight succeeds.
2. With settings publication working, run the isolated environment, Mafia
   volume=50, and custom Steam install-directory checks; verify the emitted
   profile and Wine log for each.
3. Check FPS overlay, frame generation, rumble, controller gameplay/audio, and
   clean guest exit on known-good HL2/Portal/Mafia runs.
4. For the remaining capability qualification, run a .NET title for Mono, a
   title that emits a 1076 hosted-driver event (plus GameMaker and NW.js titles
   when available), and a D3D12 title before claiming vkd3d-proton.
5. Re-run package parity and PkgTool validation for any resulting candidate.
   Keep the capability claims honest if suitable test titles are unavailable.

## Console and publication follow-up — 2026-11-19

- The user's explicit approval was to install Playden 01.26. That exact package
  was already installed and launched; its SHA-256 is
  `5b32558b72f7f1d316c284beb56fa6481ef06ffc74d490d92115f37e40ded135`.
- After the latest reboot, the console remains on the HL2 card with
  `Setup stopped early` / `Setup: Interrupted`. The captured screen is
  `/home/zpeirce/junk/codex_scratch/continuation-20261119/console-live-recheck.jpg`
  (fresh read-only Remote Play capture at 02:24 AKST).
  The newer network log
  `/home/zpeirce/junk/codex_scratch/continuation-20261119/PLDN10001-network-after-finish-setup.log`
  again reports `Before state is not a verified committed complete
  publication` for the settings request. The startup diagnostics show the
  app identity and private data root are available, but this 01.26 message does
  not identify which publication artifact failed validation.
- Playden 01.30 remains the next useful diagnostic build. Its exact installer
  on Slackian is
  `/home/zpeirce/codex_scratch/playden-profile-diagnostics-0130/package/IV0000-PLDN10001_00-PLDN100010000000.pkg`,
  SHA-256
  `19fa53425f1241044990b59d1cdd4dd294eeefcbb579cb5760ce3b76dfee1134`.
  `PkgTool.Core pkg_validate` passed all 28 checks. Parity against installed
  01.26 has zero hard failures and zero missing baseline payloads; it warns
  that matching unstripped `ntdll` and `wineserver` ELFs are absent, so their
  export and signal-mask checks are skipped. The candidate has not been
  uploaded or installed. Authorization for 01.26 does not cover this newer
  package.

## Post-reboot and publication test checkpoint — 2026-11-19 02:35 local

- The console reboot completed. A fresh capture at
  `/home/zpeirce/junk/codex_scratch/continuation-20261119/console-after-reboot.jpg`
  shows Playden on the HL2 detail page, still `Setup stopped early` / `Setup:
  Interrupted`. This was read-only; no game was launched and no console files
  were changed. The snapshot was captured despite the CLI reporting that
  ffmpeg is unavailable for its optional luma calculation.
- Added regression cases for all six 01.30 publication preflight diagnostics:
  corrupt receipt, pending receipt, missing metadata, descriptor parse error,
  app-id mismatch, and locked-content failure. Each case checks that capture
  leaves the receipt bytes unchanged. Local commit `0ef0a2c9` contains the
  tests; `sync/v0.12.0` is eight commits ahead of origin and remains unpushed.
- `runtime_settings_publication` passed under CTest (1/1, 2.24 seconds) using
  an isolated Linux host configuration. The host requires a temporary CMake
  shim because the production direct UI build requires macOS; the source tree
  and shared dependency symlinks were not changed. The test requires `umask
  022` because its admitted title-directory modes intentionally exclude 0775.
- The validated 01.30 package remains unuploaded and uninstalled. The pending
  authorization request is specifically for uploading/installing 01.30; the
  earlier approval covered 01.26 only. Do not resume console publication
  repair or capability qualification until the diagnostic package is approved
  and its specific failure is observed.

## Console package pointer recheck — 2026-11-19 02:49 local

- GoldHEN FTP port 2121 is reachable again after reboot. Read-only retrieval of
  `/user/app/PLDN10001/app.json` points to
  `/user/data/pkg/IV0000-PLDN10001_00-PLDN100010000000.pkg`, size
  1,547,501,568 bytes. That size alone does not identify the installed build.
- Read the first 64 KiB of that console package over FTP and compared it with
  the staged 01.30 candidate on Slackian. The hashes differ (`ed3c0bc6…` on
  console, `bae28895…` for 01.30); it also differs from the known 01.26,
  01.27, 01.28, and 01.29 package prefixes checked. Thus 01.30 is not the
  package currently referenced by this console path. The specific installed
  version is still unknown; FTP does not expose `eboot.bin` or PRXs from the app tree.
- The port-9300 log receiver is alive, but the latest network log is still the
  01:20 file from the earlier setup attempt. No post-reboot app launch or Wine
  log has been captured. No console files were written.
- Revalidated the exact 01.30 stage and extraction on Slackian: 6,505 files in
  each, zero missing, extra, or differing files. The candidate manifest and
  SFO both identify 01.30; the manifest records fallback commit `4965c770`,
  publication diagnostics commit `938f3192`, the 01.28 source package hash,
  and `vkd3d_proton_qualified=0`. PkgTool's saved validation log still has 28
  `[OK]` checks. This recheck does not replace the earlier 01.26 parity report.

## Console reconnect after reboot — 2026-11-19 04:10 local

- The console is back in Playden, on Settings > System. Fresh Remote Play
  capture: `/home/zpeirce/junk/codex_scratch/continuation-20261119/console-update-check.jpg`.
- The user confirmed installation of 01.26. That exact package had already
  been transferred, installed, and launched earlier in this work; no second
  installation was needed. Current Playden's exact version remains unknown:
  the on-console package pointer's content hash did not match the known
  candidates, and the UI currently shows only the idle update-check row.
- The `Check now` row was not used to install an update. The 01.30 candidate
  is still unuploaded and uninstalled. Do not substitute 01.30 under the
  01.26 authorization.
- The next capability work requiring a functional profile setup remains
  blocked by the 01.26 publication refusal (`Before state is not a verified
  committed complete publication`). 01.30 contains the detailed preflight
  reason needed to distinguish the failing artifact; it needs separate
  installation authorization before console testing can resume.

## Reinstall of the approved 01.26 baseline — 2026-11-19

- The user explicitly reconfirmed installation of Playden 01.26. After reboot,
  the Mafia card also showed `Setup stopped early` / `Never played`, and the
  package pointer hash did not match the known candidates, so the approved
  baseline was uploaded and reinstalled to re-establish a known state.
- Uploaded the exact validated package
  `IV0000-PLDN10001_00-PLDN100010000000.pkg` (SHA-256
  `5b32558b72f7f1d316c284beb56fa6481ef06ffc74d490d92115f37e40ded135`) to
  `/data/pkg/IV0000-PLDN10001_00-PLDN100010000000-0126.pkg`. FTP transfer sent
  1,547,501,568 bytes; GoldHEN FTP `LIST` reported the same size and streamed
  `RETR` hashing matched the full local SHA-256.
- GoldHEN Package Installer identified the uploaded file as Playden for PS4
  v01.26 and said the same content was already installed. The user-approved
  reinstall completed and returned to the package list. Playden was launched
  and is currently at its Home screen. This confirms 01.26 is installed now.
- No game was launched after this reinstall. The Wine log receiver remains
  listening on 10.168.2.188:9300; it has not received a new Wine session yet.
  Mafia and HL2 previously showed interrupted setup, so do not use `Finish
  setup` or alter their settings until resolving the publication refusal.
- Read-only inspection of Portal's game card after reinstall showed
  `Play — updates runtime kit first` / `Runtime kit update available`. No kit
  update was started. This is another prerequisite before Portal can serve as
  a runtime-capability test.
- 01.30 remains unapproved, unuploaded, and uninstalled. 01.26's publication
  refusal still prevents profile-setting qualification; proceed with 01.30
  only after separate approval.

## Portal runtime launch qualification — 2026-11-19

- With Playden 01.26 installed, Portal initially offered `Play — Updates runtime
  kit first`. Started that per-title runtime preparation; the card became
  `Ready`, and Playden offered to start Portal. Portal launched under
  Orbis-Win marker `GLDS10043-02.17-profile-parity`, with descriptor
  `title=FAKE78010`, `install=steam-400`, and profile `playden-steam-400`.
- The launch log records DXVK 2.6.2 and the `VK_KHR_external_memory_win32`
  extension. The display and audio sinks initialized (`video=ok`, `pad=ok`,
  `audio ... volume=100 status=ok`). Wine counters reached 1920x1080 frame
  submission and nonzero audible-callback counts. Portal reached its main menu
  over Remote Play. The controller changed the highlighted menu item, though
  the runtime's `inputs` counter remained zero; treat input capability as
  visually exercised, not fully qualified. No human audio confirmation was
  made, and this does not qualify reduced-volume behavior. No hosted-driver
  marker/event appeared in this session, so it does not qualify the
  `wine-driver-threads` claim.
- Portal was exited using its own Quit command. The log confirms
  `guest-exit status=0`, display quiesce with no pending frames, and return to
  Playden. Extracted diagnostics are at
  `/home/zpeirce/junk/codex_scratch/continuation-20261119/portal-session-0126-diagnostics.log`
  (SHA-256 `2d2ef2bec0a0b7975dc58e0711a242804fd9b0134c377feb997fa7ac8f163291`).
  The live Remote Play capture is
  `/home/zpeirce/junk/codex_scratch/continuation-20261119/portal-current.jpg`.
- After the clean exit, the Portal card showed `Play — Repairs first`,
  `Some of Portal's setup files changed`, and `Needs repair`. No repair or
  profile-setting write was attempted. This leaves environment/audio-volume/
  Steam-path qualification blocked: the known 01.26 publication preflight
  refusal makes any subsequent settings attempt ambiguous, and 01.30 remains
  unapproved. Do not use `Finish setup` or change Portal's settings until the
  publication diagnostics are resolved.
- Source review clarifies the Portal card: live status `Needs repair` is set
  when `AdmissionNeedsRepair(record->admission)` is true; “Some of Portal's
  setup files changed” is generic UI wording, not a diagnosis of changed game
  files. The `Repairs first` maintenance action calls `RepairBottle()` (which
  repairs app-owned Wine registry file modes and settles bottle modes) and
  then `RepublishSettings()`. That republish enters the same failing publication
  preflight. Keep this action untouched until the detailed 01.30 diagnostic
  identifies why the existing publication is rejected.
- Rechecked the 01.30 artifact on Slackian: the 1.5 GB PKG at
  `/home/zpeirce/codex_scratch/playden-profile-diagnostics-0130/package/IV0000-PLDN10001_00-PLDN100010000000.pkg`
  still hashes to
  `19fa53425f1241044990b59d1cdd4dd294eeefcbb579cb5760ce3b76dfee1134`.
  The candidate `playden_ui.prx` contains the specific publication reason
  strings (`receipt is invalid`, `receipt is not committed`, and
  `publication metadata is missing or unreadable`). This confirms the built
  candidate can distinguish the generic 01.26 refusal. It remains unuploaded
  and uninstalled; separate installation approval is pending.
- Read-only Playden Library inspection shows 15 installed games. It includes
  Portal and the previously surveyed HL2/Mafia/RoR2/Hollow Knight set, but not
  Cyberpunk 2077 or Starfield. The host's Steam manifests list Cyberpunk 2077
  and Starfield, both requiring DirectX 12, but neither is currently in the
  console's installed collection. Thus there is no installed D3D12 test title
  for vkd3d qualification (vendor requirements: [Cyberpunk 2077](https://support.cdprojektred.com/en/cyberpunk%20/pc/sp-technical/issue/1556/cyberpunk-2077-system-requirements),
  [Starfield](https://help.bethesda.net/app/answers/detail/a_id/60442/~/system-requirements---pc---starfield)). Capture:
  `/home/zpeirce/junk/codex_scratch/continuation-20261119/library-inspect3.jpg`.
- Rechecked the local Playden branch ancestry. `sync/v0.12.0` is eight commits
  ahead of `origin/sync/v0.12.0`; its merge base with `origin/main` is
  `ae245c2f` (the PR #4 baseline). The resulting PR diff against current
  `origin/main` is 11 files, 603 insertions, 50 deletions. No push or PR was
  made; push authorization is still separate.
- Reran `python3 tests/test_ps4_kit_view.py` on the current branch: 13/13 pass;
  `git diff --check origin/main...HEAD` is clean. The staged 01.30 kit view
  lists 17 capabilities and excludes `vkd3d-proton`; the runtime manifest
  explicitly records `vkd3d_proton_qualified=0`. The existing repo build
  directories do not register `runtime_settings_publication` with CTest; the
  earlier isolated host run (1/1) remains the only execution evidence for that
  test.
- The console log collector remains enabled on `10.168.2.188:9300`. The
  workspace filesystem had only 206 MB free when checked; avoid copying large
  packages or logs into `~/junk` until space is available.

## Playden 01.30 install and Portal smoke test — 2026-11-19

- Playden 01.30 was uploaded to the console and installed from Package
  Installer. The exact installer is
  `IV0000-PLDN10001_00-PLDN100010000000.pkg` (Slackian artifact SHA-256
  `19fa53425f1241044990b59d1cdd4dd294eeefcbb579cb5760ce3b76dfee1134`). The
  full console-side download hash matched that artifact before installation.
- Playden launched to Home and showed its 15 installed games. Portal was
  marked Ready, launched, and reached its main menu. The user selected Quit;
  the matching Wine log records `guest processes ended: returning to launcher`,
  session `b989f4ab-7022-4f5b-931e-64fcf08452c2`, `reason=guest-exit status=0`,
  and restart to the Playden launcher at 04:48:26.
- The run log identifies the packed runtime as
  `GLDS10043-02.17-profile-parity`, profile features
  `environment,audio-volume,steam-library-path`, Portal app 400, and launch
  arguments `-w 1920 -h 1080 -game portal -steam`. Video and audio sinks
  initialized; nonzero audio/audible callbacks were recorded. Remote Play
  showed the game menu. The `inputs=0` counter did not establish input telemetry,
  though the user successfully selected the in-game Quit action.
- HL2 remains `Setup stopped early` / `Interrupted`; the attempted setup action
  did not yield a new detailed publication diagnostic. Mafia also remains
  interrupted. Do not treat the old generic publication error in the retained
  network log as a fresh HL2 diagnosis. FNV was not tested with 01.30.
- Remaining hardware checks: FNV and a resolved HL2/Mafia setup path; isolated
  profile environment, audio-volume, and Steam install-path checks; a title
  that exercises hosted-driver startup; and a .NET title for Mono. vkd3d is
  still intentionally unqualified and unclaimed; no installed D3D12 test title
  is available.
- Current log collector output is
  `/home/zpeirce/junk/codex_scratch/build-ps4-logs/ps5log-20261118-203451.log`
  (about 149 MB at handoff). The two local 49 MB network-log snapshots created
  during this test are
  `/home/zpeirce/ps4-package-scratch/PLDN10001-network-test-start.log` and
  `/home/zpeirce/ps4-package-scratch/PLDN10001-network-test-after-setup.log`;
  the Playden network log remains retrievable from the console if these copies
  are later removed. Temporary `chiaki-*` screenshots from this test are in
  `/home/zpeirce/ps4-package-scratch/`.
- Before reboot, `df` reported `/home` with about 756 MB available and
  `~/junk` with about 139 MB available; both round to 100% use. The user's next
  action is to expand `~/junk`. Avoid another package or full-log copy until
  that space is available. The test run is complete and Playden is back at its
  launcher.
