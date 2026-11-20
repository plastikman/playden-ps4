# Playden PS4 v0.4.1 release handoff — 2026-11-20

## Correction from v0.4.0

The v0.4.0 CI build did not pass `WINE_MONO_DIR` or `VKD3D_PROTON_DIR` to the
packager. Since both inputs were optional, the build succeeded while omitting
Wine Mono and vkd3d-proton. The package was about 1.0 GB; the earlier 1.34
test package was about 1.5 GB and included both payloads.

This release build pins the earlier verified payloads:

- Wine Mono 11.3.0 tree SHA-256:
  `7aee0067e7aa6a43743e0b0da647d7ab6c892c7dbd288ed76f9763fc92efbaa1`
- vkd3d-proton 3.1 tree SHA-256:
  `e27aac172c62f3599ebc56f333d70ffd250b555aa0beaeb77ad81901206448c3`
- vkd3d-proton remains unqualified for D3D12 gameplay (`vkd3d_proton_qualified=0`).

The workflow now provides both pinned inputs, validates their hashes, checks
for the staged Mono and 32/64-bit vkd3d-proton files, and copies their versions
and hashes into the public build manifest. The release notes distinguish
payload presence from hardware qualification.

## Build status

- Branch: `codex/ps4-v0.4.1-release`
- Playden version: `0.4.1`
- PS4 APP_VER: `03.00`
- Orbis-Win: v0.0.8, package 02.98, SHA-256
  `6932b8532766a33afca2a386697a62b8a6c95cc2c46802fc38f185c8edb8d3bf`
- Local package passed PkgTool validation and the Orbis-Win parity gate with
  zero hard failures. The gate warns that `ntdll.shared.elf` and
  `wineserver.shared.elf` are unavailable, so their export/signal-mask checks
  are skipped.
- Local installer: `/home/zpeirce/ps4-package-scratch/playden041-release/artifacts/IV0000-PLDN10001_00-PLDN100010000000.pkg`
- Local package SHA-256:
  `2aba6120a8031d4399780db3b3458f764dfcf82f30e00e03733d53d2ea75f5ec`
- Local runtime manifest confirms the two pinned payload hashes and the
  unqualified D3D12 status.

CI publication and the console upload are still pending. Download and upload
the exact installer from `ps4-v0.4.1` after CI succeeds. Installer filename:
`IV0000-PLDN10001_00-PLDN100010000000.pkg`.
