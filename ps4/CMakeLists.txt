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
# pointed at this repository's releases. Tags are Playden's version (v<project version>).
file(STRINGS "${PLAYDEN_ROOT}/CMakeLists.txt" playden_project_line REGEX "^project\\(PlaydenLauncher VERSION ")
string(REGEX REPLACE ".*VERSION ([0-9]+\\.[0-9]+\\.[0-9]+).*" "\\1" PLAYDEN_RELEASE_VERSION "${playden_project_line}")
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
    "${PLAYDEN_ROOT}/src/steam/backend/game_path.cpp")
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

# The shipped title uses a tiny low-address eboot and keeps Playden's UI in a
# high-address PRX. The Wine PRX can then release the stub's low mapping before
# reserving the Windows guest address space, following orbis-win's tested path.
add_executable(playden_ps4_runtime_stub runtime_stub.c)
target_compile_features(playden_ps4_runtime_stub PRIVATE c_std_11)
target_compile_options(playden_ps4_runtime_stub PRIVATE -Wall -Wextra -Werror -O2 -fPIC)
