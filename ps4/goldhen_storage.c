// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "goldhen_storage.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <orbis/libkernel.h>

#define PLAYDEN_FEATURES "/data/PLDN10001/features.txt"

/* GoldHEN Plugins SDK ABI (MIT, GoldHEN/GoldHEN_Plugins_SDK): syscall 500,
 * command 2 saves the original process credentials and leaves its sandbox;
 * command 3 restores that saved state. Keep the ABI record layout local so
 * Playden does not need to link or ship the plugin SDK. */
struct goldhen_jailbreak_backup {
    uint32_t cr_uid, cr_ruid, cr_rgid, cr_groups;
    uint64_t cr_paid, cr_caps[2];
    void *cr_prison, *fd_cdir, *fd_jdir, *fd_rdir;
};

static struct goldhen_jailbreak_backup backup;
static int active;
static int exit_handler_registered;

static void log_result(const char *event, long result, int error, uid_t uid)
{
    char line[192];
    snprintf(line, sizeof(line), "PLDN10001 storage: %s result=%ld errno=%d uid=%u\n",
             event, result, error, (unsigned)uid);
    sceKernelDebugOutText(0, line);
}

/* Returns the GoldHEN syscall result, and captures FreeBSD errno from carry. */
static long goldhen_command(uint64_t command, void *data, int *error)
{
    long result;
    unsigned char failed;
    __asm__ volatile("syscall\n\tsetc %1"
                     : "=a"(result), "=q"(failed)
                     : "a"(0L), "D"(500L), "S"(command), "d"(data)
                     : "rcx", "r8", "r9", "r10", "r11", "memory", "cc");
    *error = failed ? (int)result : 0;
    return failed ? -1 : result;
}

static long guarded_command(uint64_t command, void *data, int *error)
{
    struct sigaction ignore, previous;
    memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    const int changed = sigaction(SIGSYS, &ignore, &previous) == 0;
    const long result = goldhen_command(command, data, error);
    if (changed) (void)sigaction(SIGSYS, &previous, NULL);
    return result;
}

static void restore_on_exit(void)
{
    if (!active) return;
    int error = 0;
    const long result = guarded_command(3, &backup, &error);
    log_result("GoldHEN state restored", result, error, getuid());
    active = 0;
    memset(&backup, 0, sizeof(backup));
}

static int has_opt_in(void)
{
    FILE *file = fopen(PLAYDEN_FEATURES, "rb");
    if (!file) return 0;
    char line[256];
    int enabled = 0;
    while (fgets(line, sizeof(line), file)) {
        size_t length = strlen(line);
        while (length && (line[length - 1] == '\n' || line[length - 1] == '\r' ||
                          line[length - 1] == ' ' || line[length - 1] == '\t'))
            line[--length] = '\0';
        char *start = line;
        while (*start == ' ' || *start == '\t') ++start;
        if (!strcmp(start, "external-storage")) { enabled = 1; break; }
    }
    fclose(file);
    return enabled;
}

int playden_ps4_goldhen_storage_enable_if_requested(void)
{
    if (!has_opt_in()) return 0;
    memset(&backup, 0, sizeof(backup));
    int error = 0;
    const long result = guarded_command(2, &backup, &error);
    if (result || error) {
        log_result("GoldHEN access unavailable; continuing sandboxed", result, error, getuid());
        return 0;
    }

    /* Preserve access to Playden's own 0700 files after leaving the prison. */
    const int setuid_result = setuid((uid_t)backup.cr_uid);
    const int setuid_error = setuid_result ? errno : 0;
    if (setuid_result) {
        int restore_error = 0;
        const long restore = guarded_command(3, &backup, &restore_error);
        log_result("uid restore failed; GoldHEN state rolled back", restore, restore_error, getuid());
        log_result("setuid failed", setuid_result, setuid_error, getuid());
        return 0;
    }

    active = 1;
    if (!exit_handler_registered) {
        if (atexit(restore_on_exit) != 0) {
            restore_on_exit();
            return 0;
        }
        exit_handler_registered = 1;
    }
    log_result("external storage enabled", 0, 0, getuid());
    return 1;
}

int playden_ps4_goldhen_storage_prepare_load_exec(void)
{
    const int was_enabled = active;
    if (was_enabled) restore_on_exit();
    return was_enabled;
}

void playden_ps4_goldhen_storage_resume_after_failed_load_exec(int was_enabled)
{
    if (was_enabled) (void)playden_ps4_goldhen_storage_enable_if_requested();
}
