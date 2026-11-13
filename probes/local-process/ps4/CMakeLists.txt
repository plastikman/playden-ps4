cmake_minimum_required(VERSION 3.24)
project(playden_ps4_local_process_probe LANGUAGES C)

if(NOT DEFINED ORBIS_COMPAT_DIR OR NOT PS4)
  message(FATAL_ERROR "Configure with the PS4 OpenOrbis toolchain and this checkout's orbis-compat bundle.")
endif()

set(PROBE_MAC_IP "0.0.0.0" CACHE STRING "Optional host IP for the probe's UDP log copy")

add_executable(local_process_parent ../parent.c)
target_include_directories(local_process_parent PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_options(local_process_parent PRIVATE -Wall -Wextra -Werror -O2 -fno-stack-protector)
target_compile_definitions(local_process_parent PRIVATE PROBE_MAC_IP="${PROBE_MAC_IP}")
target_link_libraries(local_process_parent PRIVATE SceSystemService)
ps4_create_eboot(local_process_parent)

add_executable(local_process_child ../child.c)
set_target_properties(local_process_child PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/child-target")
target_include_directories(local_process_child PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_options(local_process_child PRIVATE -Wall -Wextra -Werror -O2 -fno-stack-protector)
target_compile_definitions(local_process_child PRIVATE PROBE_MAC_IP="${PROBE_MAC_IP}")
ps4_create_eboot(local_process_child)

# sceSystemServiceAddLocalProcess receives a signed child image at /app0/child.bin.
add_dependencies(local_process_parent local_process_child)
ps4_create_pkg(local_process_parent
  TITLE_ID TSTP00001
  TITLE "Local Process Probe for PS4"
  VERSION 01.00
  CONTENT_LABEL LOCALPROCESS
  EXTRA_FILES "${CMAKE_CURRENT_BINARY_DIR}/child-target/eboot.bin:child.bin")
