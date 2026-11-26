#pragma once

#include "steam/install_service.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace playden::steam {

// A drive Playden may install to. `mount` must exist for the candidate to be offered;
// `games_root` is created under it. With `require_mount_point`, `mount` must also be a
// different filesystem from its parent folder: the console keeps empty /mnt/ext0 and
// /mnt/usb* folders when no drive is attached, and writing there fills system storage.
struct VolumeCandidate {
  std::string id;
  std::string label;
  std::string mount;
  std::string games_root;
  bool require_mount_point = false;
};

// Runs `work` with root rights and reports whether both succeeded.
using AsRoot = std::function<bool(const std::function<bool()>& work)>;

// Candidates whose mount exists (and is mounted, when required), with `available` set when Playden could create games_root and
// write a probe file there. The first candidate is the primary volume and is always returned,
// marked available (its folder is created on first install). A console-formatted drive's root
// belongs to root (issue #57): when the app user cannot create games_root, `as_root` creates
// the missing folders, open to everyone (0777) like an exFAT drive's, so games are still
// written by the app user.
std::vector<InstallVolume> DiscoverInstallVolumes(const std::vector<VolumeCandidate>& candidates,
                                                  const AsRoot& as_root = {});

// Candidate drives for the PS5 title: console storage, then extended storage and
// USB drives. Console storage is games_root (after the grant, the 0.6 layout's
// /data/playden-games), or title_root/games when it is empty.
std::vector<VolumeCandidate> ConsoleVolumeCandidates(const std::string& title_root = "/app0",
                                                     const std::string& games_root = {});

// PS4 USB volumes already mounted by Orbis. Call only when the user opted in
// to GoldHEN-backed external storage; these are FAT/exFAT mounts, not drivers
// Playden mounts itself.
std::vector<VolumeCandidate> Ps4UsbVolumeCandidates();

struct VolumeSpace {
  std::uint64_t total_bytes = 0;
  std::uint64_t free_bytes = 0;
};

inline constexpr std::uint64_t kInstallSpaceMargin = 64ull * 1024 * 1024;
// Console storage refuses writes (ENOSPC) while statfs still reports about 5.3 GB available: the PS5 keeps that
// for itself. Free space there leaves out a reserve: the system's share for Playden's own writes (preparations,
// updates), and more for downloads, so a finished one still leaves room to prepare the game and save.
inline constexpr std::uint64_t kConsoleSystemReserve = 6ull * 1024 * 1024 * 1024;
inline constexpr std::uint64_t kConsoleStorageReserve = 8ull * 1024 * 1024 * 1024;

// Space a download still needs: its install estimate less what it already holds on disk, which
// the drive's free space already reflects. Without a report (not started this session) the
// whole estimate is reserved.
inline std::uint64_t PendingInstallBytes(std::optional<std::uint64_t> estimate, std::uint64_t held_bytes) {
  const auto total = estimate.value_or(0);
  return total > held_bytes ? total - held_bytes : 0;
}

// Total and available bytes for the directory's drive, or nothing if it cannot be verified.
// Missing destination directories use an existing ancestor, without leaving an external root.
std::optional<VolumeSpace> QueryVolumeSpace(const std::string& path,
                                           std::uint64_t console_reserve = kConsoleStorageReserve);

// False when the platform has no way to read free space (a console whose libkernel lacks the
// query). Installs then go ahead unchecked, as before free space was readable, and a full drive
// fails the write. When it is true, a failed query means the drive itself cannot be verified.
bool VolumeSpaceSupported();

namespace detail {
// Shared validation of filesystem responses; available blocks may be negative for reserved space.
std::optional<VolumeSpace> VolumeSpaceFromBlocks(std::int64_t block_size, std::uint64_t blocks,
                                               std::int64_t available_blocks);
bool VolumeMountMatches(const std::string& path, const std::string& mount);
// Space on console storage, less a reserve.
VolumeSpace WithoutConsoleReserve(VolumeSpace space, std::uint64_t reserve);
}  // namespace detail

}  // namespace playden::steam
