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
VIEW = "runtime-template/playden-kit-view.json"
# What Orbis-Win does that Playden's settings ask a kit about (ps4/platform_ps4/game_controls.cpp Ps4Kit),
# and the any-title slots that stand for each game's profile.
CAPABILITIES = sorted(["input-keyboard-stick", "input-touchpad", "settings-dxvk-config", "title-any-id"])


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
        "capabilities": CAPABILITIES,
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
