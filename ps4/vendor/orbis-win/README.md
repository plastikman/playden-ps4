# Orbis-Win's PS4 updater, vendored

Playden checks GitHub for its own releases with Orbis-Win's PS4 updater
(libSceHttp, the package streamed to `/data/pkg`, its size and SHA-256 checked).
These files come from the Orbis-Win fork (`native/pw_update_ps4.*`,
`src/pw_update_check.*`, `src/pw_sha256.*`, from `d033126` with the
`PW_UPDATE_URL`/`PW_UPDATE_SITE`/`PW_UPDATE_AGENT` overrides) and are pointed
at this repository by the definitions in `ps4/CMakeLists.txt`.

They live here, not in `third_party/prospero-win`: that submodule is the
upstream PS5 project's, and a file added inside it cannot be committed here.
Copy a newer version over these files when Orbis-Win's updater changes.
