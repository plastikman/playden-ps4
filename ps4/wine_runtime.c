// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "wine_runtime.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <orbis/SystemService.h>
#include <orbis/libkernel.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>

enum { MODULE_INFO_BYTES = 0x160, MAX_SEGMENTS = 4, MAX_EXPORTS = 8 };
#define PRX_MAGIC UINT64_C(0x3143534544585250)
#define PLAYDEN_ROOT "/data/PLDN10001"
#define WINE_PRX "/app0/sce_module/relocated_wine_app.prx"

typedef struct { const char *name; const void *address; } Export;
typedef struct { uint64_t magic; uint32_t version, count; Export exports[]; } Descriptor;
typedef struct { const void *address; uint32_t size, protection; } Segment;
typedef void (*Entry)(int, char **);
static char last_status[192];
static int returned_from_game;  /* this start is Wine handing back after a game */
#define LAST_PLAYED PLAYDEN_ROOT "/last-played"

/* The game being launched, for the Cloud sync after it (playden_ps4_wine_take_last_played). */
static void note_last_played(uint32_t app_id)
{
    FILE *file = fopen(LAST_PLAYED, "wb");
    if (!file) return;
    fprintf(file, "%u\n", app_id);
    fflush(file);
    fsync(fileno(file));
    fclose(file);
}

int playden_ps4_wine_returned(void) { return returned_from_game; }

uint32_t playden_ps4_wine_take_last_played(void)
{
    unsigned value = 0;
    FILE *file = fopen(LAST_PLAYED, "rb");
    if (!file) return 0;
    if (fscanf(file, "%u", &value) != 1) value = 0;
    fclose(file);
    unlink(LAST_PLAYED);
    return value;
}
static char prefix_copy_error[256];
static void statusf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(last_status, sizeof(last_status), format, args);
    va_end(args);
}

const char *playden_ps4_wine_status(void) { return last_status; }

static void log_line(const char *text)
{
    sceKernelDebugOutText(0, text);
    sceKernelDebugOutText(0, "\n");
}

static int path_exists(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0;
}

static int ensure_dir(const char *path)
{
    if (mkdir(path, 0777) == 0) return 0;
    if (errno == EEXIST) {
        struct stat info;
        if (stat(path, &info) == 0 && S_ISDIR(info.st_mode)) return 0;
        // Earlier builds could create the prefix root as a regular file. Clear
        // that stale artifact so the manifest can rebuild the directory tree.
        if (stat(path, &info) == 0 && S_ISREG(info.st_mode) &&
            unlink(path) == 0 && mkdir(path, 0777) == 0) return 0;
    }
    return -1;
}

static int copy_prefix_file(const char *source, const char *destination)
{
    FILE *input = fopen(source, "rb");
    if (!input) {
        snprintf(prefix_copy_error, sizeof(prefix_copy_error), "open source %s (errno=%d)",
                 source, errno);
        return -1;
    }
    FILE *output = fopen(destination, "wb");
    if (!output) {
        snprintf(prefix_copy_error, sizeof(prefix_copy_error), "create target %s (errno=%d)",
                 destination, errno);
        fclose(input);
        return -1;
    }
    char buffer[32768];
    int failed = 0;
    for (;;) {
        size_t count = fread(buffer, 1, sizeof(buffer), input);
        if (count && fwrite(buffer, 1, count, output) != count) {
            snprintf(prefix_copy_error, sizeof(prefix_copy_error), "write target %s (errno=%d)",
                     destination, errno);
            failed = 1;
            break;
        }
        if (count < sizeof(buffer)) {
            if (ferror(input)) {
                snprintf(prefix_copy_error, sizeof(prefix_copy_error), "read source %s (errno=%d)",
                         source, errno);
                failed = 1;
            }
            break;
        }
    }
    if (fclose(input) != 0 && !failed) {
        snprintf(prefix_copy_error, sizeof(prefix_copy_error), "close source %s (errno=%d)",
                 source, errno);
        failed = 1;
    }
    if (!failed && fflush(output) != 0) {
        const int error = errno;
        snprintf(prefix_copy_error, sizeof(prefix_copy_error), "flush target %s (errno=%d)",
                 destination, error);
        failed = 1;
    }
    if (!failed && fsync(fileno(output)) != 0) {
        const int error = errno;
        snprintf(prefix_copy_error, sizeof(prefix_copy_error), "sync target %s (errno=%d)",
                 destination, error);
        failed = 1;
    }
    if (fclose(output) != 0 && !failed) {
        snprintf(prefix_copy_error, sizeof(prefix_copy_error), "close target %s (errno=%d)",
                 destination, errno);
        failed = 1;
    }
    if (failed) unlink(destination);
    return failed ? -1 : 0;
}

static int safe_prefix_relative_path(const char *path)
{
    if (!path || !path[0] || path[0] == '/' || path[0] == '\\') return 0;
    const char *component = path;
    for (const char *p = path;; ++p) {
        if (*p == '/' || *p == '\0') {
            const size_t size = (size_t)(p - component);
            if (!size || (size == 1 && component[0] == '.') ||
                (size == 2 && component[0] == '.' && component[1] == '.')) return 0;
            if (!*p) break;
            component = p + 1;
        } else if (*p == '\\' || (unsigned char)*p < 0x20) {
            return 0;
        }
    }
    return 1;
}

// refresh: the prefix was completed from an older template. Replace Wine's own files under
// drive_c/windows, add anything missing, and keep every other existing file (registry,
// user profile, saves).
static int copy_prefix_manifest(const char *manifest_path, const char *source_root,
                                const char *destination_root, int refresh)
{
    FILE *manifest = fopen(manifest_path, "rb");
    if (!manifest) {
        snprintf(prefix_copy_error, sizeof(prefix_copy_error), "open manifest %s (errno=%d)",
                 manifest_path, errno);
        return -1;
    }
    char line[2048];
    unsigned long line_number = 0;
    while (fgets(line, sizeof(line), manifest)) {
        ++line_number;
        char *newline = strpbrk(line, "\r\n");
        if (newline) *newline = '\0';
        if (!line[0]) continue;
        char *relative = NULL;
        int is_directory = 0;
        if (line[0] == 'D' && line[1] == ' ') {
            is_directory = 1;
            relative = line + 2;
        } else if (line[0] == 'F' && line[1] == ' ') {
            char *path = strchr(line + 2, ' ');
            if (path) relative = path + 1;
        }
        if (!relative || !safe_prefix_relative_path(relative)) {
            snprintf(prefix_copy_error, sizeof(prefix_copy_error), "invalid manifest line %lu",
                     line_number);
            fclose(manifest);
            return -1;
        }
        char source[1024], destination[1024];
        const int a = snprintf(source, sizeof(source), "%s/%s", source_root, relative);
        const int b = snprintf(destination, sizeof(destination), "%s/%s", destination_root, relative);
        if (a < 0 || (size_t)a >= sizeof(source) || b < 0 || (size_t)b >= sizeof(destination)) {
            snprintf(prefix_copy_error, sizeof(prefix_copy_error), "manifest path exceeds limit at line %lu",
                     line_number);
            fclose(manifest);
            return -1;
        }
        if (is_directory) {
            if (ensure_dir(destination) != 0) {
                snprintf(prefix_copy_error, sizeof(prefix_copy_error), "create directory %s (errno=%d)",
                         destination, errno);
                fclose(manifest);
                return -1;
            }
        } else if (refresh && strncmp(relative, "drive_c/windows/", 16) != 0 &&
                   path_exists(destination)) {
            continue;
        } else if (copy_prefix_file(source, destination) != 0) {
            fclose(manifest);
            return -1;
        }
    }
    if (ferror(manifest)) {
        snprintf(prefix_copy_error, sizeof(prefix_copy_error), "read manifest %s (errno=%d)",
                 manifest_path, errno);
        fclose(manifest);
        return -1;
    }
    if (fclose(manifest) != 0) {
        snprintf(prefix_copy_error, sizeof(prefix_copy_error), "close manifest %s (errno=%d)",
                 manifest_path, errno);
        return -1;
    }
    return 0;
}

static int drive_link_table_ready(const char *path)
{
    char contents[64];
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    const size_t length = fread(contents, 1, sizeof(contents), file);
    int failed = ferror(file);
    if (fclose(file) != 0) failed = 1;
    static const char expected[] = "c:\t../drive_c\nz:\t/\n";
    return !failed && length == sizeof(expected) - 1 &&
           memcmp(contents, expected, sizeof(expected) - 1) == 0;
}

static int write_drive_link_table(const char *path)
{
    char temporary[512];
    const int length = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (length < 0 || (size_t)length >= sizeof(temporary)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    FILE *file = fopen(temporary, "wb");
    if (!file) return -1;
    static const char contents[] = "c:\t../drive_c\nz:\t/\n";
    int failed = fwrite(contents, 1, sizeof(contents) - 1, file) != sizeof(contents) - 1;
    if (fflush(file) != 0) failed = 1;
    if (!failed && fsync(fileno(file)) != 0) failed = 1;
    if (fclose(file) != 0) failed = 1;
    if (failed) {
        const int saved_errno = errno ? errno : EIO;
        unlink(temporary);
        errno = saved_errno;
        return -1;
    }
    if (rename(temporary, path) != 0) {
        const int saved_errno = errno;
        unlink(temporary);
        errno = saved_errno;
        return -1;
    }
    return 0;
}

// The template's stamp (written by scripts/build-ps4-runtime.sh), as "playden-prefix-v2 <stamp>\n".
static int template_marker(char *out, size_t capacity)
{
    char stamp[96];
    FILE *file = fopen("/app0/runtime-template/hl2-prefix.stamp", "rb");
    if (!file) return -1;
    const int ok = fgets(stamp, sizeof(stamp), file) != NULL;
    fclose(file);
    if (!ok) return -1;
    stamp[strcspn(stamp, "\r\n")] = '\0';
    if (strlen(stamp) != 64) return -1;
    const int length = snprintf(out, capacity, "playden-prefix-v2 %s\n", stamp);
    return length > 0 && (size_t)length < capacity ? 0 : -1;
}

// 0: no marker, 1: marker for this template, 2: marker from an older template.
static int prefix_marker_state(const char *path, const char *expected)
{
    char contents[128];
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    const size_t length = fread(contents, 1, sizeof(contents) - 1, file);
    fclose(file);
    contents[length] = '\0';
    return strcmp(contents, expected) == 0 ? 1 : 2;
}

static int prepare_named_prefix(const char *name)
{
    char prefixes[256], prefix[320];
    char required[512], complete[384], dosdevices[384], link_table[512];
    static const char manifest[] = "/app0/runtime-template/hl2-prefix.manifest";
    static const char source[] = "/app0/runtime-template/hl2-prefix";
    if (!name || !name[0] || strchr(name, '/') || strchr(name, '\\') || strstr(name, "..")) {
        statusf("Runtime setup failed: invalid Playden prefix name.");
        return -1;
    }
    snprintf(prefixes, sizeof(prefixes), PLAYDEN_ROOT "/prefixes");
    snprintf(prefix, sizeof(prefix), "%s/%s", prefixes, name);
    snprintf(required, sizeof(required), "%s/drive_c/windows/system32/wow64native.dll", prefix);
    snprintf(complete, sizeof(complete), "%s/.playden-complete", prefix);
    snprintf(link_table, sizeof(link_table), "%s/dosdevices/.pw-symlinks", prefix);
    char expected[128];
    if (template_marker(expected, sizeof(expected)) != 0) {
        statusf("Runtime setup failed: this package's Wine prefix template has no stamp.");
        return -1;
    }
    const int marker = prefix_marker_state(complete, expected);
    if (marker == 1 && path_exists(required) && drive_link_table_ready(link_table))
        return 0;
    // A prefix completed from an older template keeps its registry and user files.
    const int refresh = marker == 2;
    prefix_copy_error[0] = '\0';
    if (refresh)
        statusf("Updating the Wine prefix '%s' for this Playden version...", name);
    else
        statusf("Preparing the Wine prefix '%s'...", name);
    if (ensure_dir(prefixes) != 0) {
        statusf("Runtime setup failed: cannot create prefix directory %s (errno=%d).",
                prefixes, errno);
        return -1;
    }
    if (ensure_dir(prefix) != 0) {
        statusf("Runtime setup failed: cannot create prefix root %s (errno=%d).", prefix, errno);
        return -1;
    }
    // A full copy starts by removing any marker, so an interrupted copy is redone in full. A
    // refresh keeps the old marker, so an interrupted refresh is retried as a refresh.
    if (!refresh) (void)unlink(complete);
    if (copy_prefix_manifest(manifest, source, prefix, refresh) != 0) {
        statusf("Runtime setup failed: could not copy prefix: %.120s.",
                prefix_copy_error[0] ? prefix_copy_error : "unknown filesystem error");
        return -1;
    }
    snprintf(dosdevices, sizeof(dosdevices), "%s/dosdevices", prefix);
    if (ensure_dir(dosdevices) != 0) {
        statusf("Runtime setup failed: cannot create directory %s (errno=%d).", dosdevices, errno);
        return -1;
    }
    if (write_drive_link_table(link_table) != 0) {
        statusf("Runtime setup failed: cannot write Wine drive mappings %s (errno=%d).",
                link_table, errno);
        return -1;
    }
    if (!path_exists(required)) {
        statusf("Runtime setup failed: prefix DLL is missing: %s.", required);
        return -1;
    }
    if (!drive_link_table_ready(link_table)) {
        statusf("Runtime setup failed: Wine drive map is invalid: %s.", link_table);
        return -1;
    }
    FILE *marker_file = fopen(complete, "wb");
    int marker_failed = marker_file == NULL;
    if (marker_file) {
        if (fputs(expected, marker_file) < 0) marker_failed = 1;
        if (fflush(marker_file) != 0) marker_failed = 1;
        if (!marker_failed && fsync(fileno(marker_file)) != 0) marker_failed = 1;
        if (fclose(marker_file) != 0) marker_failed = 1;
    }
    if (marker_failed) {
        unlink(complete);
        statusf("Runtime setup failed: cannot write prefix completion marker.");
        return -1;
    }
    statusf("Playden Wine prefix '%s' is ready.", name);
    return 0;
}

static int profile_prefix_name(uint32_t app_id, char *name, size_t capacity)
{
    char profile_path[256];
    snprintf(profile_path, sizeof(profile_path), PLAYDEN_ROOT "/profiles/%u.profile", app_id);
    FILE *profile = fopen(profile_path, "rb");
    if (!profile) return -1;
    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), profile)) {
        static const char key[] = "prefix = ";
        if (strncmp(line, key, sizeof(key) - 1) != 0) continue;
        char *value = line + sizeof(key) - 1;
        value[strcspn(value, "\r\n")] = '\0';
        const size_t length = strlen(value);
        if (length == 0 || length >= capacity || length > 80) break;
        size_t i = 0;
        for (; i < length; ++i) {
            const char ch = value[i];
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')) break;
        }
        if (i != length) break;
        memcpy(name, value, length + 1);
        found = 1;
        break;
    }
    fclose(profile);
    return found ? 0 : -1;
}

int playden_ps4_wine_prefix_path(uint32_t app_id, char *out, size_t capacity)
{
    char name[96];
    if (!out || capacity == 0 || app_id == 0 || profile_prefix_name(app_id, name, sizeof(name)) != 0) return -1;
    const int length = snprintf(out, capacity, PLAYDEN_ROOT "/prefixes/%s", name);
    return length > 0 && (size_t)length < capacity ? 0 : -1;
}

int playden_ps4_wine_prepare_game_prefix(uint32_t app_id)
{
    char name[96];
    if (app_id == 0 || profile_prefix_name(app_id, name, sizeof(name)) != 0) {
        statusf("Runtime setup failed: this game has no valid Wine prefix in its launch profile.");
        return -1;
    }
    return prepare_named_prefix(name);
}

static int prepare_hl2_prefix(char *prefix, size_t prefix_size)
{
    char path[512];
    const int length = snprintf(prefix, prefix_size,
                                PLAYDEN_ROOT "/prefixes/half-life-2");
    if (length < 0 || (size_t)length >= prefix_size) {
        statusf("Runtime setup failed: HL2 prefix path is too long.");
        return -1;
    }
    const char *required[] = {
        "system.reg",
        "drive_c/windows/system32/wow64native.dll",
        "drive_c/windows/syswow64/d3d9.dll"
    };
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        const int n = snprintf(path, sizeof(path), "%s/%s", prefix, required[i]);
        if (n < 0 || (size_t)n >= sizeof(path) || !path_exists(path)) {
            statusf("Runtime setup failed: Playden HL2 prefix is incomplete (%s).",
                    required[i]);
            return -1;
        }
    }
    if (!path_exists(PLAYDEN_ROOT "/games/220/hl2.exe")) {
        statusf("Runtime setup failed: Half-Life 2 is missing from Playden's games folder.");
        return -1;
    }
    return 0;
}

int playden_ps4_wine_prepare_hl2_install(void)
{
    statusf("Preparing Playden's Half-Life 2 prefix...");
    return prepare_named_prefix("half-life-2");
}

static int prepare_hl2(void)
{
    char prefix[256], profile_dir[256], profile[320];
    if (playden_ps4_wine_prepare_hl2_install() != 0) return -1;
    if (prepare_hl2_prefix(prefix, sizeof(prefix)) != 0) return -1;
    snprintf(profile_dir, sizeof(profile_dir), PLAYDEN_ROOT "/profiles");
    if (ensure_dir(profile_dir) != 0) {
        statusf("Runtime setup failed: cannot create the Playden profile directory.");
        return -1;
    }
    snprintf(profile, sizeof(profile), "%s/220.profile", profile_dir);
    FILE *output = fopen(profile, "wb");
    if (!output) {
        log_line("PLDN10001 runtime: cannot write HL2 launch profile");
        statusf("Runtime setup failed: cannot write the HL2 profile.");
        return -1;
    }
    fputs("[application]\n"
          "id = 220\n"
          "name = Half-Life 2\n"
          "executable = Z:\\data\\PLDN10001\\games\\220\\hl2.exe\n"
          "working_directory = Z:\\data\\PLDN10001\\games\\220\n"
          "arguments = -game hl2 -novid -fullscreen -w 1920 -h 1080\n"
          "dll_overrides = d3d8,d3d9,d3d10core,d3d11,dxgi=n\n"
          "prefix = half-life-2\n"
          "runtime = wine-wow64\n"
          "architecture = pe32\n"
          "graphics = dxvk\n\n"
          "[display]\n"
          "desktop = 1920x1080\n\n"
          "[input]\n"
          "mode = xinput\n"
          "options = escape\n"
          "mouse = touchpad\n"
          "mouse_speed = 400\n"
          "touchpad = mouse_left\n", output);
    int written = ferror(output) ? -1 : 0;
    if (fclose(output) != 0) written = -1;
    if (written != 0) statusf("Runtime setup failed: could not finish the HL2 profile.");
    return written;
}

int playden_ps4_wine_launch_hl2(void)
{
    static char runtime_mode[] = "pldn-runtime=wine";
    static char profile[] = "profile=220";
    static char cycle[] = "cycle=0";
    char *arguments[] = { runtime_mode, profile, cycle, NULL };
    statusf("Preparing Playden's Half-Life 2 prefix...");
    if (prepare_hl2() != 0) return -1;
    statusf("Requesting Wine runtime launch...");
    note_last_played(220);
    log_line("PLDN10001 runtime: restarting into Half-Life 2");
    const int status = sceSystemServiceLoadExec("/app0/eboot.bin", (const char **)arguments);
    char message[128];
    snprintf(message, sizeof(message), "PLDN10001 runtime: LoadExec returned rc=%d", status);
    log_line(message);
    statusf("LoadExec returned rc=%d; Wine did not take over.", status);
    return -1;
}

int playden_ps4_wine_launch_game(uint32_t app_id)
{
    if (app_id == 220) return playden_ps4_wine_launch_hl2();
    if (app_id == 0) {
        statusf("Runtime launch failed: invalid Steam app ID.");
        return -1;
    }
    static char runtime_mode[] = "pldn-runtime=wine";
    static char cycle[] = "cycle=0";
    char profile[40], profile_path[256];
    snprintf(profile, sizeof(profile), "profile=%u", app_id);
    snprintf(profile_path, sizeof(profile_path), PLAYDEN_ROOT "/profiles/%u.profile", app_id);
    struct stat profile_info;
    if (stat(profile_path, &profile_info) != 0 || !S_ISREG(profile_info.st_mode) || profile_info.st_size <= 0) {
        statusf("Runtime launch failed: Steam app %u has no usable Windows launch profile.", app_id);
        return -1;
    }
    if (playden_ps4_wine_prepare_game_prefix(app_id) != 0) return -1;
    char *arguments[] = { runtime_mode, profile, cycle, NULL };
    statusf("Starting the installed Steam game...");
    note_last_played(app_id);
    char line[128];
    snprintf(line, sizeof(line), "PLDN10001 runtime: launching Steam app %u", app_id);
    log_line(line);
    const int result = sceSystemServiceLoadExec("/app0/eboot.bin", (const char **)arguments);
    snprintf(line, sizeof(line), "PLDN10001 runtime: Steam app %u LoadExec returned rc=%d",
             app_id, result);
    log_line(line);
    statusf("LoadExec returned rc=%d; Wine did not take over.", result);
    return -1;
}

static int module_segments(int32_t handle, Segment out[MAX_SEGMENTS], uint32_t *count)
{
    uint8_t raw[MODULE_INFO_BYTES] __attribute__((aligned(16)));
    memset(raw, 0, sizeof(raw));
    *(uint64_t *)raw = sizeof(raw);
    if (sceKernelGetModuleInfo((OrbisKernelModule)handle,
                               (OrbisKernelModuleInfo *)raw) < 0) return -1;
    uint32_t number;
    memcpy(&number, raw + 0x148, sizeof(number));
    if (!number || number > MAX_SEGMENTS) return -1;
    for (uint32_t i = 0; i < number; ++i) {
        uint8_t *item = raw + 0x108 + i * 16;
        uint64_t address;
        memcpy(&address, item, sizeof(address));
        memcpy(&out[i].size, item + 8, sizeof(out[i].size));
        memcpy(&out[i].protection, item + 12, sizeof(out[i].protection));
        out[i].address = (const void *)(uintptr_t)address;
        if (!address || !out[i].size) return -1;
    }
    *count = number;
    return 0;
}

static const Descriptor *find_descriptor(const Segment *segments, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) {
        if (!(segments[i].protection & 1u)) continue;
        const uint8_t *base = segments[i].address;
        for (size_t offset = 0; offset + 16 <= segments[i].size; offset += 16) {
            uint64_t magic;
            memcpy(&magic, base + offset, sizeof(magic));
            if (magic != PRX_MAGIC) continue;
            const Descriptor *descriptor = (const Descriptor *)(base + offset);
            if (descriptor->version == 1 && descriptor->count && descriptor->count <= MAX_EXPORTS &&
                offset + 16 + descriptor->count * sizeof(Export) <= segments[i].size)
                return descriptor;
        }
    }
    return NULL;
}

int playden_ps4_wine_dispatch(int argc, char **argv)
{
    int requested = 0, launcher_relaunch = 0;
    /* sceSystemServiceLoadExec passes its args array starting at argv[0]. */
    for (int i = 0; argv && i < argc; ++i)
        if (!strcmp(argv[i], "pldn-runtime=wine")) requested = 1;
        else if (!strcmp(argv[i], "launcher=1")) launcher_relaunch = 1;
    returned_from_game = launcher_relaunch;
    if (!requested) {
        /* Wine returns to this eboot with launcher=1, cycle=N after the game
         * exits. That is the normal Playden UI handoff, not an error. */
        if (!launcher_relaunch && argc > 1 && argv)
            statusf("Runtime relaunch received %d args but no pldn-runtime=wine flag.", argc);
        return 0;
    }

    statusf("Runtime handoff received; loading the Wine PRX...");

    int result = 0;
    uint32_t handle = sceKernelLoadStartModule(WINE_PRX, 0, NULL, 0, NULL, &result);
    if (handle & 0x80000000u) {
        log_line("PLDN10001 runtime: Wine module load failed");
        statusf("Runtime failed: Wine PRX load returned 0x%08x.", handle);
        return 1;
    }
    Segment segments[MAX_SEGMENTS];
    uint32_t count = 0;
    const Descriptor *descriptor = NULL;
    if (module_segments((int32_t)handle, segments, &count) != 0) {
        statusf("Runtime failed: cannot inspect loaded Wine PRX (handle 0x%08x).", handle);
        return 1;
    }
    descriptor = find_descriptor(segments, count);
    Entry entry = NULL;
    if (descriptor) {
        for (uint32_t i = 0; i < descriptor->count; ++i)
            if (!strcmp(descriptor->exports[i].name, "pw_relocated_wine_start"))
                entry = (Entry)descriptor->exports[i].address;
    }
    if (!entry) {
        log_line("PLDN10001 runtime: Wine entry descriptor missing");
        statusf("Runtime failed: Wine PRX entry descriptor is missing.");
        return 1;
    }
    log_line("PLDN10001 runtime: entering bundled Wine");
    statusf("Wine PRX loaded; entering Playden's runtime...");
    entry(argc, argv);
    log_line("PLDN10001 runtime: Wine entry returned unexpectedly");
    statusf("Runtime failed: Wine entry returned unexpectedly.");
    return 1;
}
