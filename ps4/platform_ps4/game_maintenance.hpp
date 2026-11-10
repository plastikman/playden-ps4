// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace playden::ps4 {

struct DirectoryEntry {
    std::string name;
    bool directory = false;
};

// The names in a folder, without "." and "..". The title's libc opendir is refused on /data (EPERM,
// measured on 01.94) and its readdir breaks on /data's 64 KiB directory blocks (Orbis-Win, patch 0908),
// so this tries libc, then sceKernelOpen + sceKernelGetdents with a whole-block buffer. `how` names the
// one that worked, or why each failed.
bool ListDirectory(const std::string &path, std::vector<DirectoryEntry> *out, std::string *how);

}  // namespace playden::ps4

extern "C" {
// Maintenance jobs for one game, one at a time on their own thread:
//   1 back up saves: the prefix's drive_c/users, into /data/PLDN10001/user/save-backups/<app>/<time>/
//   2 restore the newest backup over the prefix's drive_c/users
//   3 reset the Wine prefix: back up, then delete everything but drive_c/users; the next launch makes
//     Windows and the registry again from the template, and the saves stay.
//   4 before an uninstall that removes the prefix: back up, then delete the whole prefix
// 0 when it started.
int playden_ps4_maintenance_start(std::uint32_t app_id, int action);
// 0 idle, 1 running, 2 done, 3 failed.
int playden_ps4_maintenance_state(void);
const char *playden_ps4_maintenance_message(void);
// "Last backup 2026-11-10 21:05 UTC, 12 MB", or "" when the game has none.
const char *playden_ps4_maintenance_backup_label(std::uint32_t app_id);
}
