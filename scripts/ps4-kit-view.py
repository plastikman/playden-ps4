#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Describe the Orbis-Win bundled in the PS4 package as a Playden runtime kit.

On the PS5, Playden's runtime (src/homebrew/runtime_host.cpp) admits an installed prospero-win kit
from its manifest. The PS4 package carries Orbis-Win itself, so the PS4 build describes it instead:
playden-kit-view.json lists what Playden's package installer copies per game (the template prefix
into each game's bottle, and a few small files into each game's title folder) with each file's
size, SHA-256 and mode. ps4/runtime_ps4/kit_ps4.cpp reads it into a VerifiedKitView.

Usage: ps4-kit-view.py STAGE KIT_ID RUNTIME_REVISION WINE_REVISION
Everything is written under STAGE/runtime-template (paths in the view are relative to STAGE, the
package root /app0).
"""

import hashlib
import json
import os
import shutil
import sys
from pathlib import Path

TEMPLATE = "runtime-template/hl2-prefix"
TITLE = "runtime-template/kit/title"
TITLE_ICON = TITLE + "/sce_sys/playden-icon.png"
RUNTIME = "runtime-template/kit/runtime"
RECIPE = "runtime-template/kit/recipes/orbis-win-ps4.json"
AUDIO_DRIVER = "sce_module/wineps5.prx"
# The WGL/Zink OpenGL provider, built by Orbis-Win's build_mesa_zink.sh and staged for
# "graphics = opengl" games. Both architectures must be present to claim the capability.
ZINK_PROVIDERS = ("win/mesa-zink/i386-windows/opengl32.dll", "win/mesa-zink/x86_64-windows/opengl32.dll")
VIEW = "runtime-template/playden-kit-view.json"
# What Orbis-Win does that Playden's settings ask a kit about (ps4/platform_ps4/game_controls.cpp Ps4Kit),
# and the any-title slots that stand for each game's profile.
# ⚠ Do NOT add game-root-unowned here. It reads like "this kit can start a game from a folder it
# does not own", which is what an FTP-imported game needs (GoldHEN writes as root, the importer
# places the tree with rename, so the game arrives uid 0 while Playden runs as uid 1). It is not
# per-game: the capability makes Playden publish EVERY game folder unowned
# (settings_publication.h), so declaring it re-publishes the whole library at startup. Tried in
# 03.38 on 2026-11-18: every regeneration was refused with "keyboard_stick: unknown movement
# stick" and Playden crashed on its title screen. An imported game needs to be owned by Playden
# instead -- the importer should copy a tree it does not own rather than rename it.
# profile-version-2: Playden gates its "32-bit code" row (cpu = native|translator) on this, and a
# kit that declares it writes "version = 2" into [runtime] beside the cpu line. Orbis-Win has read
# cpu all along (prospero-win src/pw_game_profile.c), but until it also accepted "version" an
# unknown key in that section refused the whole profile -- so declaring this used to break every
# game. The runtime this kit ships now accepts it (upstream PR #753); do not move this line ahead
# of a runtime that does. Without it the row reads "Needs a newer runtime kit" and an imported
# game cannot be switched to the translator: MDK, a 1997 32-bit title, crashed on native with
# c0000005 five seconds in (2026-11-18).
# Claim only what the runtime's profile parser actually reads (pw_game_profile.c). A capability is a
# promise about behaviour, not about shipping a file: game-root-unowned crashed the launcher because
# it is kit-wide, and display-120hz must stay off this list even though [display] refresh takes 120 --
# it promises the runtime paces the cap itself via [display] frame_rate, which the parser does not
# know, and Playden turns OFF its software frame limiter for a kit that claims it (SoftwareFrameLimit,
# game_settings.cpp), so games would run with no cap at all.
#
# Audited against kit_trust.cpp's 34 known capabilities on 2026-11-18. Still unclaimable because the
# parser has no section for them: settings-profile-environment ([environment]), settings-audio-volume
# ([audio]), helper-process ([helper]), steam-library-path ([steam]). Partial, so left off:
# settings-input-tuning (stick_deadzone yes, trigger_deadzone no), input-playstation (no such [input]
# mode). Environment variables already work without settings-profile-environment: that row's gate also
# passes on opengl-zink, and the parser reads [debug] env.
# Every capability this kit claims, with the evidence for it. A claim is a promise about
# BEHAVIOUR, not about shipping a file, and the dangerous mistake is not "claiming something we do
# not support" -- it is claiming something that makes Playden disable a working fallback or emit a
# profile section Orbis-Win cannot read. See NEVER_CLAIM below for the ones that bite.
#
# providers: staged files that must all exist, or () when the provider is the runtime's profile
# parser (prospero-win src/pw_game_profile.c) rather than a file in the package.
CAPABILITY_EVIDENCE = {
    # The parser reads these keys; see pw_game_profile.c section_of() and the *_field() functions.
    "profile-version-2": ((), "[runtime] version = 2, beside the cpu line"),
    "settings-dxvk-config": ((), "DXVK reads dxvk.conf from the game's working directory"),
    "title-any-id": ((), "one title shell serves every game"),
    "input-keyboard-stick": ((), "[input] keyboard_stick = left_stick|right_stick"),
    "input-touchpad": ((), "[input] touchpad"),
    "input-rumble": ((), "[input] rumble"),
    "display-framegen": ((), "[display] framegen = 2|3|4|off"),
    "display-fps-counter": ((), "[display] show_fps, which Playden writes for every profile"),
    # Playden routes a "graphics = opengl" game through Mesa's Zink only when the kit says it can
    # (kKitOpenGLZink, game_settings.h). PS4 ships that provider, built by Orbis-Win's
    # build_mesa_zink.sh -- but the kit never claimed it, so an OpenGL game got a window and no
    # rendering (The Binding of Isaac, 2026-11-12) because Playden fell back to a PS5 OpenGL SDK
    # path this console does not have. Both architectures must be staged.
    "opengl-zink": (ZINK_PROVIDERS, "win/mesa-zink/*/opengl32.dll"),
}

# Capabilities that must stay unclaimed, and why. Keeping the reason in code is the point: each of
# these was either tried and reverted, or is unreachable, and a future audit should not re-derive it.
NEVER_CLAIM = {
    "display-120hz":
        "Promises the RUNTIME paces the frame cap itself via [display] frame_rate, which the parser "
        "does not know. Playden's SoftwareFrameLimit() turns OFF its software limiter for a kit that "
        "claims this, so games would run with no cap at all. [display] refresh taking 120 is not enough.",
    "game-root-unowned":
        "Not per-game: it makes Playden publish EVERY game folder unowned (settings_publication.h), "
        "re-publishing the whole library at startup. Tried in 03.38 on 2026-11-18: every regeneration "
        "was refused and Playden crashed on its title screen. Import by copying a tree we own instead.",
    "display-vrr":
        "Has no KitCapabilities field at all -- read straight from the manifest, it flips param.json "
        "attribute3 bit 0x40000 on every title and invalidates them, and that bit is a PS5 attribute. "
        "Our WSI always submits ORBIS_VIDEO_OUT_FLIP_VSYNC at 60Hz (src/native/pw_videoout_ps4.c).",
    "settings-dxvk-present":
        "Every present is a VSYNC flip; there is no immediate/mailbox path, so VSync off cannot be "
        "honoured (measured, results/20261005-dxvk-conf-probe).",
    "wine-large-address-aware":
        "Wine implements it (patch 1040 reads WINE_LARGE_ADDRESS_AWARE) but no profile can deliver a "
        "WINE_* variable: [debug] env's family allowlist excludes it. Worse, claiming it makes Playden "
        "emit an [environment] section regardless of kit.environment (game_settings.cpp:1651-1699), "
        "which this parser refuses outright. Needs [environment] first.",
    "nsiproxy-switch": "Same [environment] emission trap as wine-large-address-aware, and no switch exists.",
    "environment-file-budgets":
        "Wine implements the budgets (patch 1056) but WINE_PS5_* names are silently dropped by the "
        "[debug] env allowlist, so claiming it only removes an honest refusal. Needs [environment].",
    "bottle-profile-shared":
        "Admission requires exactly 0700 (pw_title_publication_native.c); claiming it triggers a chmod "
        "sweep to 0711 across all installs and would break every bottle, irreversibly in place.",
    "publication-open-modes":
        "Sets strict_modes = 0 in Playden's own pre-flight, so a title reports admissible while the "
        "shell refuses it at launch and the diagnostic that would name the fault says 'ok'.",
    "settings-input-tuning": "Partial: the parser has stick_deadzone but not trigger_deadzone.",
    "input-playstation": "No such [input] mode value in the parser.",
    "game-root-usb-search": "/mnt is ENOENT inside the PS4 title sandbox; there is nothing to search.",
    "settings-profile-environment":
        "No [environment] section in the parser; Playden already routes the Environment variables "
        "row through [debug] env because opengl-zink is claimed.",
    "settings-audio-volume":
        "No [audio] section in the parser. The consumer is close -- pw_audio_ps5_open_port() sets "
        "all eight channels to 0dB unconditionally -- but the profile cannot carry a volume yet.",
    "helper-process":
        "SCM_RIGHTS fails EINVAL on PS4 and a local child has its own descriptor table, so the child "
        "cannot be handed the Wine-server socket the PS5 helper's SendFd() design depends on. "
        "See docs/HANDOFF-2026-11-18-PS4-NATIVE-STEAM.md.",
    "steam-library-path": "No [steam] section in the parser, and no bottle-side steamapps tree.",
    "title-notify": "Needs Leo's pw_title_notify.h, which we do not have; also dead without helper-process.",
    "launcher-mode": "No [launcher] section; our exec chain is sequential, the opposite of what it needs.",
    "touchpad-switch": "Needs [launcher] too -- claiming it alone still makes Playden emit that section.",
    "launch-arguments-4096":
        "Raises the WHOLE profile cap to 16KiB, not just arguments; our buffers are still the old tier "
        "(PW_APP_ARGUMENTS_CAPACITY 513, PW_APP_PROFILE_MAX_BYTES 8192).",
    "settings-input-wheel": "No [input] wheel key; the wheel exists only from a real USB mouse. Needs patch 0745.",
}


def capabilities(stage) -> list:
    """The capabilities whose evidence is actually present in this stage.

    A claim with staged providers is dropped when any of them is missing, so the kit cannot promise
    something this build does not carry. Claims whose provider is the profile parser are listed
    unconditionally -- the parser ships inside the runtime, not as a file we can stat here -- and
    tests/test_ps4_kit_view.py holds them against prospero-win's actual section and field lists.
    """
    found = [name for name, (providers, _) in CAPABILITY_EVIDENCE.items()
             if all((stage / provider).is_file() for provider in providers)]
    return sorted(found)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def main() -> int:
    if len(sys.argv) != 5:
        print(__doc__, file=sys.stderr)
        return 2
    stage = Path(sys.argv[1])
    kit_id, runtime_revision, wine_revision = sys.argv[2:5]

    # The console drive table Playden's installer requires in the template (and Orbis-Win reads).
    write(stage / TEMPLATE / "dosdevices/.pw-symlinks", "c:\t../drive_c\nz:\t/\n")
    # A game's title folder holds only its parameters, icon and launch descriptor: Playden writes the
    # game's own param.json and icon over these (FilesystemPackageInstaller::TitleMappings).
    write(stage / TITLE / "sce_sys/param.json", json.dumps({
        "attribute3": 64,
        "conceptId": "78000",
        "contentId": "UP9000-FAKE78000_00-PROSPEROWIN00001",
        "localizedParameters": {"en-US": {"titleName": "Playden game"}},
        "titleId": "FAKE78000",
    }, indent=2, sort_keys=True) + "\n")
    # PkgTool reserves a nested icon0.png as a package icon and omits it from the app0 PFS.
    # Use an ordinary source name; the PS4 package installer maps it to sce_sys/icon0.png.
    shutil.copyfile(Path(__file__).resolve().parents[1] / "assets/branding/generic-title-icon.png",
                    stage / TITLE_ICON)
    write(stage / TITLE / "orbis-win-title.txt", "Played by PLDN10001's bundled Orbis-Win.\n")
    write(stage / RUNTIME / "runtime-inventory.json",
          json.dumps({"orbis-win": runtime_revision, "wine": wine_revision}, sort_keys=True) + "\n")
    write(stage / RECIPE, json.dumps({"id": "orbis-win-ps4", "version": "1", "launch_entries": []},
                                     sort_keys=True) + "\n")

    files = []
    for root in (TEMPLATE, TITLE, RUNTIME, os.path.dirname(RECIPE)):
        for path in sorted((stage / root).rglob("*")):
            if path.is_symlink() or not path.is_file():
                continue
            # PkgTool makes regular files in app0 read-only with mode 0555, regardless of
            # their host mode. Record the mode the console actually sees: admission and
            # the installer both check the package source before copying it.
            path.chmod(0o644)
            relative = path.relative_to(stage).as_posix()
            files.append({"path": relative, "size": path.stat().st_size, "sha256": sha256(path), "mode": 0o555})
    files.sort(key=lambda item: item["path"])
    canonical = "".join(f"{f['path']}\t{f['size']}\t{f['sha256']}\t{f['mode']:04o}\n" for f in files)
    recipe_sha = next(f["sha256"] for f in files if f["path"] == RECIPE)
    view = {
        "schema": "playden-ps4-kit-view/1",
        "kit_id": kit_id,
        "files_sha256": hashlib.sha256(canonical.encode()).hexdigest(),
        "capabilities": capabilities(stage),
        "runtime": {"bundle_id": "wine-wow64", "artifact_revision": runtime_revision, "path": RUNTIME,
                    "wine_revision": wine_revision},
        # mmdevapi loads Wine's audio driver by name, and a recipe may require the "ps5" one --
        # the Source-engine recipe steam-220-hl2-v1 (HL2, HL:Source, L4D2, Portal) does. PS4 ships
        # that same driver as sce_module/wineps5.prx, built by Orbis-Win's wineps5-prx-ps4.sh, so
        # say so when it is actually in the stage. The console admits this kit from this file
        # (ps4/runtime_ps4/kit_ps4.cpp) instead of re-deriving it from the template's registry the
        # way kit_manifest.cpp does on PS5, so without this the capability reads as absent and
        # every such recipe is refused with "The installed runtime kit lacks what this game needs
        # (PS5 audio)" -- which made "Update games to the new runtime kit" fail permanently and
        # left Recreate bottle as the only recovery.
        "template": {"path": TEMPLATE, "drive_table": TEMPLATE + "/dosdevices/.pw-symlinks",
                     "audio_ps5": (stage / AUDIO_DRIVER).is_file()},
        "recipe": {"id": "orbis-win-ps4", "version": "1", "path": RECIPE, "sha256": recipe_sha},
        "variants": [{"id": "ps4", "title_id": "FAKE78000", "path": TITLE, "any_title": True}],
        "files": files,
    }
    write(stage / VIEW, json.dumps(view, indent=1, sort_keys=True) + "\n")
    print(f"kit view: {len(files)} files, kit {kit_id}, runtime {runtime_revision}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
