cmake_minimum_required(VERSION 3.24)
project(playden_ps4_shell LANGUAGES C CXX)
add_library(playden_ps4_app_objects OBJECT main.c wine_runtime.c)
set_target_properties(playden_ps4_app_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(playden_ps4_app_objects PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(playden_ps4_app_objects PRIVATE c_std_11)
target_compile_options(playden_ps4_app_objects PRIVATE -Wall -Wextra -Werror -O2)
add_executable(playden_ps4_shell shell_entry.c $<TARGET_OBJECTS:playden_ps4_app_objects>)
set_target_properties(playden_ps4_shell PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(playden_ps4_shell PRIVATE c_std_11)
target_compile_options(playden_ps4_shell PRIVATE -Wall -Wextra -Werror -O2)
target_link_libraries(playden_ps4_shell PRIVATE SceVideoOut SceSystemService SceUserService ScePad m)
set_target_properties(playden_ps4_shell PROPERTIES OUTPUT_NAME playden-ps4)

# PS4 implementations of the pinned Hui UI's display and controller boundary.
# This target is kept independent from the bring-up shell so it can be linked
# into the Playden UI executable after the Mesa EGL path is validated.
set(PLAYDEN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
# PS4 controller implementation of the pinned Hui UI's input boundary.
add_library(playden_ps4_ui_platform STATIC
    platform_ps4/pad.cpp
    platform_ps4/system.cpp)
target_include_directories(playden_ps4_ui_platform PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PLAYDEN_ROOT}/third_party/ps5-homebrew-ui/src")
target_compile_features(playden_ps4_ui_platform PUBLIC cxx_std_20)
target_compile_options(playden_ps4_ui_platform PRIVATE -Wall -Wextra -Werror -O2 -fno-exceptions -fno-rtti)
set_target_properties(playden_ps4_ui_platform PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Renderer-neutral Hui draw lists and font layout, rasterized into the native
# VideoOut buffer. This keeps launcher display ownership on the proven path.
set(HUI_ROOT "${PLAYDEN_ROOT}/third_party/ps5-homebrew-ui")
add_library(playden_ps4_hui STATIC
    ui.cpp
    "${PLAYDEN_ROOT}/src/homebrew/artwork.cpp"
    "${PLAYDEN_ROOT}/src/homebrew/artwork_cache.cpp"
    "${PLAYDEN_ROOT}/src/homebrew/store_art.cpp"
    "${PLAYDEN_ROOT}/src/homebrew/qr.cpp"
    "${PLAYDEN_ROOT}/third_party/qrcodegen/qrcodegen.c"
    "${HUI_ROOT}/src/gfx/draw_list.cpp"
    "${HUI_ROOT}/src/gfx/font.cpp"
    "${HUI_ROOT}/src/gfx/triangulate.cpp"
    "${HUI_ROOT}/src/ui/glyphs.cpp")
target_include_directories(playden_ps4_hui PRIVATE
    "${HUI_ROOT}/src" "${PLAYDEN_ROOT}/src" "${PLAYDEN_ROOT}/third_party/stb")
target_compile_features(playden_ps4_hui PRIVATE cxx_std_20)
target_compile_options(playden_ps4_hui PRIVATE -Wall -Wextra -Werror -O2 -fno-exceptions -fno-rtti)
set_target_properties(playden_ps4_hui PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(playden_ps4_shell PRIVATE playden_ps4_hui)

# Existing Playden Steam QR-auth implementation with PS4-built curl/Mbed TLS.
find_package(Threads REQUIRED)
set(PS4_NETWORK_PREFIX "${PLAYDEN_ROOT}/scripts/network-deps/.deps/ps4")
foreach(required_path IN ITEMS
        "${PS4_NETWORK_PREFIX}/include/curl/curl.h"
        "${PS4_NETWORK_PREFIX}/lib/libcurl.a"
        "${PS4_NETWORK_PREFIX}/cacert.pem")
    if(NOT EXISTS "${required_path}")
        message(FATAL_ERROR "PS4 Steam networking dependency missing: ${required_path}. Run scripts/network-deps/prepare-ps4-deps.sh")
    endif()
endforeach()
add_library(playden_ps4_curl STATIC IMPORTED GLOBAL)
set_target_properties(playden_ps4_curl PROPERTIES
    IMPORTED_LOCATION "${PS4_NETWORK_PREFIX}/lib/libcurl.a"
    INTERFACE_INCLUDE_DIRECTORIES "${PS4_NETWORK_PREFIX}/include"
    INTERFACE_COMPILE_DEFINITIONS CURL_STATICLIB)
add_library(playden_ps4_mbedtls STATIC IMPORTED GLOBAL)
set_target_properties(playden_ps4_mbedtls PROPERTIES IMPORTED_LOCATION "${PS4_NETWORK_PREFIX}/lib/libmbedtls.a")
add_library(playden_ps4_mbedx509 STATIC IMPORTED GLOBAL)
set_target_properties(playden_ps4_mbedx509 PROPERTIES IMPORTED_LOCATION "${PS4_NETWORK_PREFIX}/lib/libmbedx509.a")
add_library(playden_ps4_mbedcrypto STATIC IMPORTED GLOBAL)
set_target_properties(playden_ps4_mbedcrypto PROPERTIES IMPORTED_LOCATION "${PS4_NETWORK_PREFIX}/lib/libmbedcrypto.a")
add_library(playden_ps4_everest STATIC IMPORTED GLOBAL)
set_target_properties(playden_ps4_everest PROPERTIES IMPORTED_LOCATION "${PS4_NETWORK_PREFIX}/lib/libeverest.a")
add_library(playden_ps4_p256m STATIC IMPORTED GLOBAL)
set_target_properties(playden_ps4_p256m PROPERTIES IMPORTED_LOCATION "${PS4_NETWORK_PREFIX}/lib/libp256m.a")

add_library(playden_ps4_steam_auth STATIC
    platform_ps4/steam_auth.cpp
    "${PLAYDEN_ROOT}/src/steam/backend/auth_client.cpp"
    "${PLAYDEN_ROOT}/src/steam/backend/password_crypto.cpp"
    "${PLAYDEN_ROOT}/src/steam/backend/auth_messages.cpp"
    "${PLAYDEN_ROOT}/src/steam/backend/proto_wire.cpp"
    "${PLAYDEN_ROOT}/src/steam/backend/json.cpp"
    "${PLAYDEN_ROOT}/src/steam/backend/token_claims.cpp"
    "${PLAYDEN_ROOT}/src/steam/backend/library_client.cpp"
    "${PLAYDEN_ROOT}/src/steam/backend/entitled_library.cpp"
    "${PLAYDEN_ROOT}/src/steam/backend/family_groups.cpp"
    "${PLAYDEN_ROOT}/src/steam/backend/app_names.cpp"
    "${PLAYDEN_ROOT}/src/steam/store_details_service.cpp"
    "${PLAYDEN_ROOT}/src/library/store.cpp"
    "${PLAYDEN_ROOT}/src/library/cache.cpp"
    "${PLAYDEN_ROOT}/src/library/service.cpp"
    "${PLAYDEN_ROOT}/src/library/organization.cpp"
    "${PLAYDEN_ROOT}/src/steam/backend/http_client.cpp"
    "${PLAYDEN_ROOT}/src/steam/backend/native_network.cpp"
    "${PLAYDEN_ROOT}/src/steam/backend/native_http_ps4.cpp"
    "${PLAYDEN_ROOT}/src/steam/backend/ws_client.cpp"
    "${PLAYDEN_ROOT}/src/steam/backend/cm_client.cpp"
    "${PLAYDEN_ROOT}/src/steam/backend/cm_friends_source.cpp"
    "${PLAYDEN_ROOT}/src/steam/backend/cm_messages.cpp"
    "${PLAYDEN_ROOT}/src/steam/backend/app_info_http.cpp"
    "${PLAYDEN_ROOT}/src/steam/backend/app_info.cpp"
    "${PLAYDEN_ROOT}/src/steam/backend/vdf.cpp"
    "${PLAYDEN_ROOT}/src/steam/backend/content_language.cpp"
    "${PLAYDEN_ROOT}/src/steam/backend/binary_kv.cpp"
    "${PLAYDEN_ROOT}/src/steam/backend/gzip.cpp"
    "${PLAYDEN_ROOT}/src/platform/effective_uid.cpp"
    "${PLAYDEN_ROOT}/src/platform/storage.cpp"
    "${PLAYDEN_ROOT}/src/steam/service.cpp"
    "${PLAYDEN_ROOT}/src/steam/friends_service.cpp"
    "${PLAYDEN_ROOT}/src/steam/session_store.cpp"
    "${PLAYDEN_ROOT}/src/installs/install_record.cpp"
    "${PLAYDEN_ROOT}/third_party/prospero-win/src/pw_app_profile.c"
    "${PLAYDEN_ROOT}/third_party/prospero-win/src/pw_profile_catalog.c")
target_include_directories(playden_ps4_steam_auth PUBLIC
    "${PLAYDEN_ROOT}/src" "${PLAYDEN_ROOT}/third_party" "${PLAYDEN_ROOT}/third_party/stb"
    "${PLAYDEN_ROOT}/third_party/prospero-win/src"
    "${PLAYDEN_ROOT}/third_party/prospero-win/include"
    "${CMAKE_CURRENT_SOURCE_DIR}" "${PS4_NETWORK_PREFIX}/include")
target_compile_features(playden_ps4_steam_auth PUBLIC cxx_std_20)
target_compile_options(playden_ps4_steam_auth PRIVATE -Wall -Wextra -O2 -fno-rtti)
set_target_properties(playden_ps4_steam_auth PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(playden_ps4_steam_auth PRIVATE PLAYDEN_PLATFORM_PS4=1)
target_link_libraries(playden_ps4_steam_auth PUBLIC
    playden_ps4_curl playden_ps4_mbedtls playden_ps4_mbedx509 playden_ps4_mbedcrypto
    playden_ps4_everest playden_ps4_p256m Threads::Threads SceNet SceSysmodule SceImeDialog)
target_link_libraries(playden_ps4_shell PRIVATE playden_ps4_steam_auth)

# Reuse Playden's existing verified depot downloader for PS4 installs. Steam CM remains the
# source of license grants and depot keys; CDN requests use HttpClient's PS4 libSceHttp adapter.
set(PLAYDEN_ZSTD_DIR "${PLAYDEN_ROOT}/third_party/zstd")
add_library(playden_ps4_depot_codec STATIC
    "${PLAYDEN_ROOT}/src/steam/backend/depot_chunk.cpp"
    "${PLAYDEN_ROOT}/src/steam/backend/depot_manifest.cpp"
    "${PLAYDEN_ROOT}/third_party/lzma-sdk/LzmaDec.c"
    "${PLAYDEN_ZSTD_DIR}/common/debug.c"
    "${PLAYDEN_ZSTD_DIR}/common/entropy_common.c"
    "${PLAYDEN_ZSTD_DIR}/common/error_private.c"
    "${PLAYDEN_ZSTD_DIR}/common/fse_decompress.c"
    "${PLAYDEN_ZSTD_DIR}/common/xxhash.c"
    "${PLAYDEN_ZSTD_DIR}/common/zstd_common.c"
    "${PLAYDEN_ZSTD_DIR}/decompress/huf_decompress.c"
    "${PLAYDEN_ZSTD_DIR}/decompress/zstd_ddict.c"
    "${PLAYDEN_ZSTD_DIR}/decompress/zstd_decompress.c"
    "${PLAYDEN_ZSTD_DIR}/decompress/zstd_decompress_block.c")
target_include_directories(playden_ps4_depot_codec PRIVATE
    "${PLAYDEN_ROOT}/src"
    "${PS4_NETWORK_PREFIX}/include"
    "${PLAYDEN_ROOT}/third_party/lzma-sdk"
    "${PLAYDEN_ZSTD_DIR}"
    "${PLAYDEN_ZSTD_DIR}/common"
    "${PLAYDEN_ROOT}/third_party/stb")
target_compile_features(playden_ps4_depot_codec PUBLIC cxx_std_20)
target_compile_definitions(playden_ps4_depot_codec PRIVATE
    ZSTD_DISABLE_ASM=1 ZSTD_LEGACY_SUPPORT=0 ZSTD_TRACE=0)
target_compile_options(playden_ps4_depot_codec PRIVATE -O2 -fno-rtti)
set_target_properties(playden_ps4_depot_codec PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(playden_ps4_steam_install STATIC
    "${PLAYDEN_ROOT}/src/steam/backend/cdn_client.cpp"
    "${PLAYDEN_ROOT}/src/steam/backend/cm_app_details.cpp"
    "${PLAYDEN_ROOT}/src/steam/backend/depot_installer.cpp"
    "${PLAYDEN_ROOT}/src/steam/backend/steam_install_runner.cpp"
    "${PLAYDEN_ROOT}/src/installs/install_service.cpp"
    "${PLAYDEN_ROOT}/src/steam/install_volumes.cpp"
    "${PLAYDEN_ROOT}/src/runtime/steamworks.cpp"
    "${PLAYDEN_ROOT}/src/runtime/artifact_hash.cpp"
    "${PLAYDEN_ROOT}/src/platform/managed_directory.cpp"
    "${PLAYDEN_ROOT}/src/platform/managed_file_lock.cpp")
target_include_directories(playden_ps4_steam_install PUBLIC
    "${PLAYDEN_ROOT}/src" "${PLAYDEN_ROOT}/third_party/stb")
target_compile_features(playden_ps4_steam_install PUBLIC cxx_std_20)
target_compile_definitions(playden_ps4_steam_install PRIVATE PLAYDEN_PLATFORM_PS4=1)
target_compile_options(playden_ps4_steam_install PRIVATE -Wall -Wextra -O2 -fno-rtti)
set_target_properties(playden_ps4_steam_install PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(playden_ps4_steam_install PUBLIC
    playden_ps4_steam_auth playden_ps4_depot_codec Threads::Threads)
target_link_libraries(playden_ps4_shell PRIVATE playden_ps4_steam_install)

# Playden's own update: Orbis-Win's PS4 updater (libSceHttp, streamed to /data/pkg, SHA-256 checked),
# pointed at this repository's releases. Tags are the PS4 port's own version, v<ps4/VERSION> (0.0.1 and up),
# not upstream's project version: a release named after Leo's (0.11.0) would never look newer to the port.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" PLAYDEN_RELEASE_VERSION LIMIT_COUNT 1)
if(NOT PLAYDEN_RELEASE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "ps4/VERSION must be MAJOR.MINOR.PATCH, not '${PLAYDEN_RELEASE_VERSION}'")
endif()
add_library(playden_ps4_update STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/orbis-win/native/pw_update_ps4.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/orbis-win/src/pw_update_check.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/orbis-win/src/pw_sha256.c")
target_include_directories(playden_ps4_update PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/vendor/orbis-win")
target_compile_definitions(playden_ps4_update PRIVATE
    PW_RELEASE_VERSION="${PLAYDEN_RELEASE_VERSION}"
    PW_UPDATE_URL="https://api.github.com/repos/plastikman/playden-ps4/releases/latest"
    PW_UPDATE_SITE="github.com/plastikman/playden-ps4"
    PW_UPDATE_AGENT="playden-ps4"
    PW_UPDATE_ASSET="IV0000-PLDN10001_00-PLDN100010000000.pkg")
target_compile_options(playden_ps4_update PRIVATE -O2)
set_target_properties(playden_ps4_update PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(playden_ps4_shell PRIVATE playden_ps4_update)
target_include_directories(playden_ps4_hui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/orbis-win")
target_include_directories(playden_ps4_app_objects PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/orbis-win")

# Playden's shared per-game settings model: generated recipes, the player's saved choices and the
# launch profile they render (ps4/platform_ps4/game_controls.cpp).
add_library(playden_ps4_miniz STATIC "${PLAYDEN_ROOT}/third_party/miniz/miniz.c")
target_include_directories(playden_ps4_miniz PUBLIC "${PLAYDEN_ROOT}/third_party/miniz")
target_compile_definitions(playden_ps4_miniz PUBLIC
    MINIZ_NO_TIME MINIZ_NO_ZLIB_COMPATIBLE_NAMES MINIZ_NO_ARCHIVE_WRITING_APIS)
set_target_properties(playden_ps4_miniz PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(playden_ps4_game_settings STATIC
    platform_ps4/game_controls.cpp
    platform_ps4/game_maintenance.cpp
    "${PLAYDEN_ROOT}/src/runtime/game_settings.cpp"
    "${PLAYDEN_ROOT}/src/runtime/prospero_profile.cpp"
    "${PLAYDEN_ROOT}/src/runtime/game_probe.cpp"
    "${PLAYDEN_ROOT}/src/runtime/recipe_ini.cpp"
    "${PLAYDEN_ROOT}/src/runtime/recipe_files.cpp"
    "${PLAYDEN_ROOT}/src/runtime/install_script.cpp"
    "${PLAYDEN_ROOT}/src/runtime/msi_package.cpp"
    "${PLAYDEN_ROOT}/src/runtime/lzx_decoder.cpp"
    "${PLAYDEN_ROOT}/src/runtime/burn_bundle.cpp"
    "${PLAYDEN_ROOT}/src/runtime/redist_install.cpp"
    "${PLAYDEN_ROOT}/src/runtime/steam_stub.cpp"
    "${PLAYDEN_ROOT}/src/steam/backend/game_path.cpp"
    "${PLAYDEN_ROOT}/src/library/game_ref.cpp"
    "${PLAYDEN_ROOT}/src/library/query.cpp"
    "${PLAYDEN_ROOT}/third_party/utf8proc/utf8proc.c")
# game_ref.cpp (recipe game names) folds text with utf8proc, as the PS5 build's playden_library does.
target_include_directories(playden_ps4_game_settings PRIVATE "${PLAYDEN_ROOT}/third_party/utf8proc")
target_compile_definitions(playden_ps4_game_settings PRIVATE UTF8PROC_STATIC)
target_include_directories(playden_ps4_game_settings PUBLIC
    "${PLAYDEN_ROOT}/src" "${PLAYDEN_ROOT}/third_party" "${PLAYDEN_ROOT}/third_party/stb"
    "${CMAKE_CURRENT_SOURCE_DIR}" "${PS4_NETWORK_PREFIX}/include")
target_compile_features(playden_ps4_game_settings PUBLIC cxx_std_20)
target_compile_definitions(playden_ps4_game_settings PRIVATE PLAYDEN_PLATFORM_PS4=1)
target_compile_options(playden_ps4_game_settings PRIVATE -Wall -Wextra -O2 -fno-rtti)
set_target_properties(playden_ps4_game_settings PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(playden_ps4_game_settings PUBLIC
    playden_ps4_steam_auth playden_ps4_steam_install playden_ps4_miniz Threads::Threads)
target_link_libraries(playden_ps4_shell PRIVATE playden_ps4_game_settings)

# Reuse Playden's existing conflict-aware Steam Cloud engine for the known-good HL2 prefix.
add_library(playden_ps4_steam_cloud STATIC
    "${PLAYDEN_ROOT}/src/steam/backend/cloud_sync.cpp"
    "${PLAYDEN_ROOT}/src/steam/backend/cloud_mapping.cpp"
    "${PLAYDEN_ROOT}/src/steam/backend/cloud_messages.cpp"
    "${PLAYDEN_ROOT}/src/steam/backend/steam_cloud_runner.cpp")
target_include_directories(playden_ps4_steam_cloud PUBLIC "${PLAYDEN_ROOT}/src")
target_compile_features(playden_ps4_steam_cloud PUBLIC cxx_std_20)
target_compile_definitions(playden_ps4_steam_cloud PRIVATE PLAYDEN_PLATFORM_PS4=1)
target_compile_options(playden_ps4_steam_cloud PRIVATE -Wall -Wextra -O2 -fno-rtti)
set_target_properties(playden_ps4_steam_cloud PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(playden_ps4_steam_cloud PUBLIC playden_ps4_steam_auth playden_ps4_depot_codec Threads::Threads)
target_link_libraries(playden_ps4_shell PRIVATE playden_ps4_steam_cloud)
target_include_directories(playden_ps4_hui PRIVATE
    "${PLAYDEN_ROOT}/src" "${PLAYDEN_ROOT}/third_party/qrcodegen")

# Playden's own UI (src/homebrew) with Hui's renderer replaced by a CPU one (hui_cpu/, whose gfx/
# headers shadow hui's OpenGL ones): a measurement mode behind /data/PLDN10001/ui-preview.
file(GLOB PLAYDEN_PS4_HUI_WIDGETS CONFIGURE_DEPENDS "${HUI_ROOT}/src/ui/*.cpp" "${HUI_ROOT}/src/ui/components/*.cpp")
list(REMOVE_ITEM PLAYDEN_PS4_HUI_WIDGETS "${HUI_ROOT}/src/ui/glyphs.cpp")  # in playden_ps4_hui
file(GLOB PLAYDEN_PS4_COMMUNITY CONFIGURE_DEPENDS "${PLAYDEN_ROOT}/src/community/*.cpp")
add_library(playden_ps4_homebrew_preview STATIC
    homebrew_preview.cpp
    hui_cpu/cpu_renderer.cpp
    ${PLAYDEN_PS4_HUI_WIDGETS}
    "${HUI_ROOT}/src/core/input.cpp"
    "${PLAYDEN_ROOT}/src/homebrew/simulation.cpp"
    "${PLAYDEN_ROOT}/src/homebrew/storage_sim.cpp"
    "${PLAYDEN_ROOT}/src/homebrew/queue_layout.cpp"
    "${PLAYDEN_ROOT}/src/homebrew/browse.cpp"
    "${PLAYDEN_ROOT}/src/homebrew/view.cpp"
    "${PLAYDEN_ROOT}/src/homebrew/title_icon.cpp"
    "${PLAYDEN_ROOT}/src/app/diagnostic_log.cpp"
    "${PLAYDEN_ROOT}/src/app/preferences.cpp"
    "${PLAYDEN_ROOT}/src/diagnostics/redaction.cpp"
    "${PLAYDEN_ROOT}/src/platform/random_id.cpp"
    "${PLAYDEN_ROOT}/src/platform/durable_storage.cpp"
    "${PLAYDEN_ROOT}/src/runtime/my_presets.cpp"
    "${PLAYDEN_ROOT}/src/runtime/community_rules.cpp"
    "${PLAYDEN_ROOT}/src/runtime/recipe_choice.cpp"
    "${PLAYDEN_ROOT}/src/runtime/private_directories.cpp"
    "${PLAYDEN_ROOT}/src/runtime/kit_version.cpp"
    "${PLAYDEN_ROOT}/src/update/asset_download.cpp"
    "${PLAYDEN_ROOT}/src/update/release.cpp"
    "${PLAYDEN_ROOT}/src/update/version.cpp"
    "${PLAYDEN_ROOT}/src/update/zip_extract.cpp"
    "${PLAYDEN_ROOT}/src/update/file_ops.cpp"
    ${PLAYDEN_PS4_COMMUNITY})
target_include_directories(playden_ps4_homebrew_preview BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/hui_cpu")
target_include_directories(playden_ps4_homebrew_preview PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${HUI_ROOT}/src" "${PLAYDEN_ROOT}/src" "${PLAYDEN_ROOT}/third_party"
    "${PLAYDEN_ROOT}/third_party/stb" "${HUI_ROOT}/third_party/stb" "${PLAYDEN_ROOT}/third_party/qrcodegen"
    "${PLAYDEN_ROOT}/third_party/miniz" "${PS4_NETWORK_PREFIX}/include")
target_compile_features(playden_ps4_homebrew_preview PRIVATE cxx_std_20)
# No thread-local storage in stb_image: the PRX loader does not resolve dynamic TLS on worker threads.
# update/version.cpp: the PS4 port's own version (ps4/VERSION) and the commit, as the desktop build
# writes them (cmake/WriteBuildCommit.cmake).
find_package(Git QUIET)
set(PLAYDEN_PS4_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${PLAYDEN_ROOT}" "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
    "-DOUTPUT=${PLAYDEN_PS4_GENERATED}/playden_build_commit.h" -P "${PLAYDEN_ROOT}/cmake/WriteBuildCommit.cmake")
target_include_directories(playden_ps4_homebrew_preview PRIVATE "${PLAYDEN_PS4_GENERATED}")
target_compile_definitions(playden_ps4_homebrew_preview PRIVATE PLAYDEN_VERSION="${PLAYDEN_RELEASE_VERSION}")
# The SDK's dirent names the inode d_fileno, without FreeBSD's d_ino alias.
target_compile_definitions(playden_ps4_homebrew_preview PRIVATE PLAYDEN_PLATFORM_PS4=1 STBI_NO_THREAD_LOCALS
    d_ino=d_fileno)
target_compile_options(playden_ps4_homebrew_preview PRIVATE -O2 -fno-exceptions -fno-rtti)
set_target_properties(playden_ps4_homebrew_preview PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(playden_ps4_homebrew_preview PUBLIC playden_ps4_ui_platform playden_ps4_game_settings)

# Step 3: the rest of the service graph Playden's desktop host (src/homebrew/host.cpp) links in live mode,
# so Leo's Controller and services run unchanged on the PS4. Lists follow cmake/HomebrewUI.cmake,
# PlaydenCore.cmake, Web.cmake and ProsperoRuntime.cmake; third-party code already built for the
# PS4 (zstd, lzma, miniz, qrcodegen, utf8proc, pw_app_profile, pw_profile_catalog) is not repeated.
set(PLAYDEN_PS4_WEB_ASSETS_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/web_assets.cpp")
set(PLAYDEN_PS4_WEB_FONTS "${PLAYDEN_ROOT}/fixtures/ui-comparison/assets/fonts")
set(PLAYDEN_PS4_WEB_ASSETS
    "/|html|${PLAYDEN_ROOT}/src/web/page/index.html"
    "/assets/barlow-regular.ttf|ttf|${PLAYDEN_PS4_WEB_FONTS}/Barlow-Regular.ttf"
    "/assets/barlow-semibold.ttf|ttf|${PLAYDEN_PS4_WEB_FONTS}/Barlow-SemiBold.ttf"
    "/assets/barlow-OFL.txt|text|${PLAYDEN_PS4_WEB_FONTS}/barlow-OFL.txt")
add_custom_command(OUTPUT "${PLAYDEN_PS4_WEB_ASSETS_CPP}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${PLAYDEN_PS4_WEB_ASSETS_CPP}" "-DASSETS=${PLAYDEN_PS4_WEB_ASSETS}"
        -P "${PLAYDEN_ROOT}/cmake/EmbedWebAssets.cmake"
    DEPENDS "${PLAYDEN_ROOT}/src/web/page/index.html" "${PLAYDEN_ROOT}/cmake/EmbedWebAssets.cmake"
    VERBATIM)
set(PW_SRC "${PLAYDEN_ROOT}/third_party/prospero-win")
add_library(playden_ps4_publication STATIC
    # The PS4's managed title folders live under /data/PLDN10001, so this one file is the
    # vendored copy (ps4/vendor/orbis-win/README.md): the submodule is upstream's and cannot
    # carry a PS4 change.
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/orbis-win/native/pw_title_publication_native.c"
    "${PW_SRC}/src/pw_title_publication.c"
    "${PW_SRC}/src/pw_title_launch.c"
    "${PW_SRC}/src/pw_game_profile.c"
    "${PW_SRC}/src/pw_wine_launch.c"
    "${PW_SRC}/src/pw_publication_sha256.c"
    "${PW_SRC}/third_party/mbedtls-publication/library/sha256.c"
    "${PLAYDEN_ROOT}/third_party/sqlite3/sqlite3.c")
target_include_directories(playden_ps4_publication PRIVATE "${PW_SRC}/native" "${PW_SRC}/src"
    "${PW_SRC}/third_party/mbedtls-publication" "${PW_SRC}/third_party/mbedtls-publication/include"
    "${PLAYDEN_ROOT}/third_party/sqlite3")
target_compile_definitions(playden_ps4_publication PRIVATE _DEFAULT_SOURCE PLAYDEN_PLATFORM_PS4=1
    MBEDTLS_CONFIG_FILE="pw_publication_config.h"
    SQLITE_OMIT_LOAD_EXTENSION=1 SQLITE_THREADSAFE=0 SQLITE_TEMP_STORE=3 SQLITE_MAX_MMAP_SIZE=0 SQLITE_USE_URI=0)
target_compile_options(playden_ps4_publication PRIVATE -O2)
set_target_properties(playden_ps4_publication PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(PLAYDEN_PS4_LIVE_SOURCES
    app/experimental_features.cpp app/library_controller.cpp app/persistence_controller.cpp app/persistence_service.cpp
    homebrew/artwork_backend.cpp homebrew/controller.cpp homebrew/gog_icon.cpp homebrew/program_icon.cpp
    homebrew/runtime_host.cpp homebrew/sharing_service.cpp homebrew/uninstall_flow.cpp
    installs/game_move.cpp library/update_check.cpp
    platform/ps5/native_coordinator.cpp platform/ps5/title_activity.cpp platform/ps5/title_allow_list.cpp
    platform/ps5/title_database.cpp platform/ps5/title_registry.cpp platform/ps5/title_remover.cpp
    platform/ps5/title_runtime.cpp
    runtime/dotnet_heap.cpp runtime/game_install.cpp runtime/install_service.cpp runtime/launch_service.cpp
    runtime/layout_migration.cpp runtime/leftover_titles.cpp runtime/package_installer.cpp
    runtime/publication_backup.cpp runtime/publication_contract.cpp runtime/publication_transaction.cpp
    runtime/self_heal.cpp runtime/session_reconciler.cpp runtime/session_storage.cpp runtime/settings_publication.cpp
    runtime/settings_publication_journal.cpp runtime/settings_publication_owner.cpp runtime/settings_storage.cpp
    runtime/steam_input.cpp runtime/title_parameters.cpp runtime/writable_folders.cpp
    steam/app_details_service.cpp steam/backend/download_server_source.cpp steam/cloud_service.cpp
    steam/download_server_service.cpp steam/local_session_repository.cpp
    stores/local/copy_plan.cpp stores/local/copy_runner.cpp stores/local/drop_folder_access.cpp
    stores/local/import_store.cpp stores/local/library_source.cpp stores/local/scanner.cpp stores/registry.cpp
    update/app_swap.cpp update/update_service.cpp
    web/address.cpp web/file_policy.cpp web/files_api.cpp web/file_store.cpp web/http.cpp web/logs_source.cpp
    web/pairing.cpp web/router.cpp web/server.cpp web/service.cpp web/zip.cpp)
list(TRANSFORM PLAYDEN_PS4_LIVE_SOURCES PREPEND "${PLAYDEN_ROOT}/src/")
add_library(playden_ps4_homebrew_live STATIC homebrew_live.cpp runtime_ps4/kit_ps4.cpp
    runtime_ps4/legacy_bottle.cpp
    ${PLAYDEN_PS4_LIVE_SOURCES} "${PLAYDEN_PS4_WEB_ASSETS_CPP}")
target_include_directories(playden_ps4_homebrew_live PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${HUI_ROOT}/src" "${PLAYDEN_ROOT}/src" "${PLAYDEN_ROOT}/third_party"
    "${PLAYDEN_ROOT}/third_party/stb" "${HUI_ROOT}/third_party/stb" "${PLAYDEN_ROOT}/third_party/miniz"
    "${PLAYDEN_ROOT}/third_party/sqlite3" "${PLAYDEN_ROOT}/third_party/utf8proc"
    "${PW_SRC}/src" "${PW_SRC}/native" "${PW_SRC}/third_party/mbedtls-publication"
    "${PW_SRC}/third_party/mbedtls-publication/include" "${PS4_NETWORK_PREFIX}/include" "${PLAYDEN_PS4_GENERATED}")
target_compile_features(playden_ps4_homebrew_live PRIVATE cxx_std_20)
target_compile_definitions(playden_ps4_homebrew_live PRIVATE PLAYDEN_PLATFORM_PS4=1 STBI_NO_THREAD_LOCALS
    MBEDTLS_CONFIG_FILE="pw_publication_config.h" PLAYDEN_VERSION="${PLAYDEN_RELEASE_VERSION}")
target_compile_options(playden_ps4_homebrew_live PRIVATE -O2 -fno-exceptions -fno-rtti
    -include "${CMAKE_CURRENT_SOURCE_DIR}/compat/dirent_ino.h")
# These two catch exceptions (std::filesystem / parsing), as on the PS5.
set_source_files_properties("${PLAYDEN_ROOT}/src/app/persistence_service.cpp"
    "${PLAYDEN_ROOT}/src/runtime/package_installer.cpp" TARGET_DIRECTORY playden_ps4_homebrew_live
    PROPERTIES COMPILE_OPTIONS "-fexceptions")
set_target_properties(playden_ps4_homebrew_live PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(playden_ps4_homebrew_live PUBLIC playden_ps4_publication playden_ps4_homebrew_preview)

# The shipped title uses a tiny low-address eboot and keeps Playden's UI in a
# high-address PRX. The Wine PRX can then release the stub's low mapping before
# reserving the Windows guest address space, following orbis-win's tested path.
add_executable(playden_ps4_runtime_stub runtime_stub.c)
target_compile_features(playden_ps4_runtime_stub PRIVATE c_std_11)
target_compile_options(playden_ps4_runtime_stub PRIVATE -Wall -Wextra -Werror -O2 -fPIC)
