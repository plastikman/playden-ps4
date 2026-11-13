# PS4 handoff — 2026-11-13

Supersedes `docs/ps4-current-handoff-2026-11-12.md` for the HL2 thread. Everything in
that document about the build recipe, the capability table and the stale-build trap
still stands; read it first.

## What is on the console right now

| Piece | Build | Identity |
| --- | --- | --- |
| Orbis-Win | 0210 | `7fd7a602…`, FNV verified **37.3 fps in-world**, 0 per-page exec entries |
| Playden | 02.85 | `773471da…`, kit `orbis-win-ps4-4a89b3920e30`, uploaded as `IV0000-PLDN10001_00-PLDN100010000000-0285.pkg` |

Orbis-Win 0210 is good: Zak drove FNV into the world on it today and called it good.
The open problem is Playden-side only.

## The HL2 blocker — root cause found

Half-Life 2 (`/data/PLDN10001/games/220`) will not launch. The UI refuses with
*"installed title and bottle are not this install's from a compatible kit"*, and
Recreate-bottle + Update-runtime-kit both "succeed" without changing the refusal.

The real reason is not in the UI text. It is in `/data/PLDN10001-network.log`:

```
PLDN10001 live: runtime: prepare result app=220 The installed runtime kit lacks what
this game needs (PS5 audio); install a newer runtime
```

That is the `audio=ps5` recipe requirement, refused by
`src/runtime/package_installer.cpp:494`:

```cpp
if (name == "audio" && value == "ps5" && kit.template_audio_ps5) continue;
```

`kit.template_audio_ps5` is false. **Measured, do not re-derive:**

1. `sce_module/wineps5.prx` **is** in the 0210 stage (183,664 B) — the driver is shipped.
2. `playden-kit-view.json` **does** say `"template": {"audio_ps5": true, …}` —
   `scripts/ps4-kit-view.py:125` sets it from `(stage / "sce_module/wineps5.prx").is_file()`.
3. Neither of those is what the C++ consults. `template_audio_ps5` is written in exactly
   one non-fixture place, `src/runtime/kit_manifest.cpp:504`:
   ```cpp
   view_.template_audio_ps5 = audio || ValidateRegistry(template_root, true);
   ```
   and `ValidateRegistry(bottle, require_audio=true)` ends
   (`src/runtime/kit_manifest.cpp:215`) with:
   ```cpp
   return !require_audio || RegistrySectionValue(*user, R"(Software\\Wine\\Drivers)", R"("Audio"="ps5")");
   ```
4. The shipped template prefix has no such key. In
   `build/ps4-runtime/pkg-stage/runtime-template/hl2-prefix/user.reg` there is **no
   `[Software\Wine\Drivers]` section at all** and **no `"Audio"=` line anywhere**.

So: the capability is a property of the **template prefix's registry**, not of the stage's
file list. Shipping `wineps5.prx` does not grant it, and the generator script's
`audio_ps5` JSON field is testing the wrong thing — the file — and is (as far as the
greps go) never read back into `VerifiedKitView` on PS4.

### Why the kit still admits

`kit_manifest.cpp:482` calls `ValidateRegistry(template_root, audio)` with the kit's *own*
recipe's audio requirement. Our kit recipe does not ask for `audio=ps5`, so admission
passes; line 504 then re-tests with `true` for the benefit of *other* recipes Playden
ships, and that is the test that fails. The kit is valid — it just advertises one fewer
capability than HL2 needs. This matches the 11-12 handoff note that "every `audio=ps5`
recipe refused… the kit update could never succeed for HL2/HL:Source/L4D2/Portal".

## Follow-up: Playden 02.86 (2026-11-13)

Keep the earlier `programs=0` report separate from this audio failure. The former
was traced to the v0.0.6 Orbis-Win runtime lacking the launch-descriptor reader;
the runtime source was moved to `4a89b392` with the four Playden integrations,
including `1f53082` for descriptors under the title's own data root. The 02.85
audio refusal is a separate kit-template mismatch.

The audio key belongs in the template: `GameRecipe::requirements` explicitly
describes what the kit template must provide, and `audio=ps5` is an admission
check rather than a per-game registry mutation. Added the registry write to
`scripts/build-ps4-runtime.sh` immediately after `wineboot --init` and before
registry canonicalization. Playden commit: `b0e3321`.

Built Playden 02.86 from `ps4/upstream-ports` in the isolated worktree
`/home/zpeirce/codex_scratch/playden-0286`. The staged template contains
`"Audio"="ps5"`; the generated view still advertises `audio_ps5: true`. The
installer is
`IV0000-PLDN10001_00-PLDN100010000000-0286.pkg`, SHA-256
`0c7b3d58c57b945b5f692ee02ade69a3fb8526b917f02216abe7c61eda55d3d4`.

Package parity against 02.85 exited **0 (0 FAIL)**. The candidate contains all
baseline payload files, and all Orbis-Win runtime PRXs match the baseline.
Export/signal-mask and `PW_EXCEPTION32` reachability checks warned because the
02.10 release artifact has no matching unstripped `ntdll`/`wineserver` ELFs or
kernelbase source tree. The remaining warnings and disposition are recorded in
`/home/zpeirce/codex_scratch/playden-0286/build/ps4-runtime/parity-disposition-0286.txt`.

Uploaded the versioned installer to
`/data/pkg/IV0000-PLDN10001_00-PLDN100010000000-0286.pkg` (1,001,783,296 B).
It has not yet been installed or tested on-console. This session does not expose
a PS4 package-install MCP tool; install it from the PS4 package installer, then
update or recreate the HL2 bottle if Playden reports its template hash is stale.
The template change alters `files_sha256`, so existing bottle ownership markers
may need updating even though the kit ID is unchanged. Then verify `programs=`
is non-zero and retry HL2. The fresh klog and coredump watchers remain armed under
`/home/zpeirce/codex_scratch/ps4-hl2-20261113-045453/`.

## Still open, unchanged from yesterday

- `0680` — the last Leo patch never attempted.
- Two `0964` diagnostic strings missing vs the `playden-orbis-0173` baseline (parity WARN).
- Hollow Knight stops at Unity first scene load.
- Playden's 02.86 template audio change is committed as `b0e3321`; its package
  is uploaded but not installed. Keep the runtime source and prior package/build
  provenance in their respective handoffs.
- Moving the runtime commit invalidates existing game installs — nine games prompted for
  a kit update and HL2 needed a full Recreate. That is a release decision, not a
  packaging detail.

## Follow-up: Playden 02.87 descriptor runtime build (2026-11-13)

Built a Playden package with the committed Orbis-Win Playden runtime group from
`codex/v006-playden-runtime` at `47f0cefb4e5c39aad0e60e7a3ba1dfcc50376adf`.
The app PRX was compiled on Slackian from the recovered v0.0.6 Wine build
snapshot. The Playden source was `ps4/upstream-ports` at `73a7e5b` (the
02.86 handoff commit); no Playden source code changed for this package.

The 02.87 package uses the exact Orbis-Win 02.10 runtime payload from
`candidate-0210`, matching the 02.86 package. It changes the relocated app PRX
and kit/version metadata only. The Playden UI PRX, icon, Wine template and
`Audio=ps5` registry setting were preserved byte-for-byte from 02.86. The new
kit ID is `orbis-win-ps4-47f0cefb4e5c-1ef8823a3838`.

The installer is
`IV0000-PLDN10001_00-PLDN100010000000-0287.pkg`, SHA-256
`5ddde23aa575f7f3dafefbecb2be5fb5b20343a54b86366942a1043875cda324`,
1,001,783,296 bytes. Package parity against 02.86 exited 0 with no missing or
added payload files; `PkgTool.Core pkg_validate` passed all 27 checks. The
package was uploaded to `/data/pkg/` and read back; the console copy matched
the local SHA-256 exactly. It has **not yet been installed or tested**.

The parity report warns that ntdll/wineserver export and signal-mask checks
were skipped because their matching unstripped 02.10 ELFs are unavailable.
Those PRXs were reused unchanged from the 02.86 baseline; their package copies
match byte-for-byte. The 32-bit `kernelbase.dll` contains 25 `PW_EXCEPTION32`
strings, and the no-debugger path reachability check passes. Build reports and
the warning disposition are under
`/home/zpeirce/codex_scratch/playden-runtime-build-0287/`.

Installing 02.87 changes the runtime kit ID. Treat existing game kit markers
as stale and follow Playden's update/recreate prompts before testing HL2.
The klog, coredump, PLDN10001 and ps5log watchers remain armed.
