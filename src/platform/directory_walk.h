#pragma once
// Descriptor walks used by permission repair; never fall back to privileged path operations.
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#if defined(__PROSPERO__) || (defined(PLAYDEN_PLATFORM_PS5) && PLAYDEN_PLATFORM_PS5) || \
    (defined(PLAYDEN_PLATFORM_PS4) && PLAYDEN_PLATFORM_PS4)
// A PS5 title can't open relative to a directory descriptor: its openat/mkdirat take only
// AT_FDCWD (any real descriptor is EINVAL), and fchdir crashes it. Each step opens the absolute
// path without following its last component, then checks that what it opened is the entry the
// held parent descriptor lists under that name, so a link or folder swapped in above is refused.
#define PLAYDEN_WALK_BY_PATH 1
#endif

namespace playden::platform {
// Strict: an entry must be the one the parent lists (privileged walks). ReadOnlyFiles: a regular
// file that fails only the inode comparison is still accepted. exFAT/msdosfs numbers an empty file
// (no clusters) differently in readdir than in stat, so every 0-byte file in a game fails the
// comparison. Use it only where the walk reads as the app user, which can't gain anything by
// swapping in a file of its own; directories always keep the strict check.
enum class WalkTrust { kStrict, kReadOnlyFiles };

// The by-path walk is compiled everywhere so host tests can drive it; only PS5 builds use it.
namespace walk_detail {
struct Opened {
  std::filesystem::path path;
  dev_t device;
  ino_t inode;
};
inline std::mutex& Lock() {
  static std::mutex lock;
  return lock;
}
// The path each walked descriptor was opened at. A closed descriptor's entry stays until the
// number is reused; the identity check below refuses a reused number for another folder.
inline std::unordered_map<int, Opened>& Paths() {
  static std::unordered_map<int, Opened> paths;
  return paths;
}
inline void Remember(int fd, const std::filesystem::path& path) {
  struct stat info{};
  if (fd < 0 || ::fstat(fd, &info) != 0 || !S_ISDIR(info.st_mode)) return;
  std::lock_guard<std::mutex> lock(Lock());
  Paths()[fd] = {path, info.st_dev, info.st_ino};
}
inline bool PathOf(int fd, std::filesystem::path* path) {
  struct stat info{};
  if (::fstat(fd, &info) != 0) return false;
  std::lock_guard<std::mutex> lock(Lock());
  const auto found = Paths().find(fd);
  if (found == Paths().end() || found->second.device != info.st_dev || found->second.inode != info.st_ino) {
    errno = EBADF;
    return false;
  }
  *path = found->second.path;
  return true;
}
// The same, from the folder's path. On the PS4 a title may not list a folder through a descriptor
// it already holds (fdopendir, and the getdents behind it, are refused), while listing the path
// works; with no listing every child read as a swapped entry and the walk refused it (ESTALE).
inline std::uint64_t ListedInodeAtPath(const std::filesystem::path& parent_path, const char* name) {
  DIR* listing = ::opendir(parent_path.c_str());  // the parent's own path; see OpenHeldListing
  if (!listing) return 0;
  std::uint64_t inode = 0;
  while (const auto* entry = ::readdir(listing))
    if (std::string(entry->d_name) == name) { inode = entry->d_fileno; break; }
  ::closedir(listing);
  return inode;
}
// The inode the parent's listing gives `name`, or 0 when it has none.
inline std::uint64_t ListedInode(int parent, const char* name) {
  const int listing_fd = ::dup(parent);
  DIR* listing = listing_fd >= 0 ? ::fdopendir(listing_fd) : nullptr;
  if (!listing) {
    if (listing_fd >= 0) ::close(listing_fd);
    return 0;
  }
  std::uint64_t inode = 0;
  while (const auto* entry = ::readdir(listing))
    if (std::string(entry->d_name) == name) { inode = entry->d_fileno; break; }
  ::closedir(listing);
  return inode;
}
inline bool ValidName(const char* name) {
  const std::string text = name ? name : "";
  return !text.empty() && text != "." && text != ".." && text.find('/') == std::string::npos;
}

// `listed_inode`: the inode the caller read from the parent's listing, saving a second scan.
inline int OpenChildByPath(int parent, const char* name, int flags, std::uint64_t listed_inode, WalkTrust trust) {
  std::filesystem::path parent_path;
  if (!walk_detail::ValidName(name)) { errno = EINVAL; return -1; }
  if (!walk_detail::PathOf(parent, &parent_path)) return -1;
  struct stat parent_info{};
  if (::fstat(parent, &parent_info) != 0) return -1;
  const auto path = parent_path / name;
  const int fd = ::open(path.c_str(), flags | O_NOFOLLOW, 0);
  if (fd < 0) return -1;
  struct stat opened{};
  bool same = ::fstat(fd, &opened) == 0;
  // Across a mount point the listing shows the covered folder's inode, and the entries of / are
  // system folders (firmlinks on macOS): O_NOFOLLOW still refused a link there, and the app user
  // can't plant a mount or replace an entry of /.
  if (same && opened.st_dev == parent_info.st_dev && parent_path != "/") {
    const std::uint64_t listed = listed_inode ? listed_inode
                                              : walk_detail::ListedInodeAtPath(parent_path, name);
    same = listed != 0 && listed == static_cast<std::uint64_t>(opened.st_ino);
  }
  // Opened without following links and on the parent's device, so a regular file is accepted.
  if (!same && trust == WalkTrust::kReadOnlyFiles && S_ISREG(opened.st_mode) && opened.st_dev == parent_info.st_dev)
    same = true;
  if (!same) {
    ::close(fd);
    errno = ESTALE;
    return -1;
  }
  Remember(fd, path);
  return fd;
}
}  // namespace walk_detail

#if defined(PLAYDEN_WALK_BY_PATH)
inline int OpenWalkChild(int parent, const char* name, int flags, std::uint64_t listed_inode = 0) {
  return walk_detail::OpenChildByPath(parent, name, flags, listed_inode, WalkTrust::kStrict);
}
inline int OpenWalkChildUnprivileged(int parent, const char* name, int flags, std::uint64_t listed_inode = 0) {
  return walk_detail::OpenChildByPath(parent, name, flags, listed_inode, WalkTrust::kReadOnlyFiles);
}
inline int MakeWalkChild(int parent, const char* name, mode_t mode) {
  std::filesystem::path parent_path;
  if (!walk_detail::ValidName(name)) { errno = EINVAL; return -1; }
  if (!walk_detail::PathOf(parent, &parent_path)) return -1;
  // The parent path must still name the held parent: mkdir never follows its last component.
  struct stat held{}, named{};
  if (::fstat(parent, &held) != 0) return -1;
  if (::lstat(parent_path.c_str(), &named) != 0) return -1;
  if (!S_ISDIR(named.st_mode) || named.st_dev != held.st_dev || named.st_ino != held.st_ino) {
    errno = ESTALE;
    return -1;
  }
  return ::mkdir((parent_path / name).c_str(), mode);
}
#else
inline int OpenWalkChild(int parent, const char* name, int flags, std::uint64_t = 0) {
  return ::openat(parent, name, flags);
}
inline int OpenWalkChildUnprivileged(int parent, const char* name, int flags, std::uint64_t = 0) {
  return ::openat(parent, name, flags);
}
inline int MakeWalkChild(int parent, const char* name, mode_t mode) {
  return ::mkdirat(parent, name, mode);
}
#endif
// A listing of a folder already held open and checked. By descriptor where that works, so the
// names come from the very folder the caller holds; a PS4 title cannot dup a directory descriptor
// (EPERM, measured on PLDN10001 02.66), so there it lists the path, which the caller has already
// walked without following links. Null on failure, with errno set; close it with closedir.
inline DIR* OpenHeldListing(int fd, const std::filesystem::path& path) {
#if defined(PLAYDEN_WALK_BY_PATH)
  (void)fd;
  return ::opendir(path.c_str());
#else
  (void)path;
  const int duplicate = ::dup(fd);
  if (duplicate < 0) return nullptr;
  DIR* listing = ::fdopendir(duplicate);
  if (!listing) {
    const int error = errno;
    ::close(duplicate);
    errno = error;
  }
  return listing;
#endif
}

// Starts at the filesystem root, opening each component without following links.
inline int OpenDirectoryWalk(const std::filesystem::path& path) {
  if (!path.is_absolute() || path.string().find('\0') != std::string::npos) { errno = EINVAL; return -1; }
  int fd = ::open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
#if defined(PLAYDEN_WALK_BY_PATH)
  walk_detail::Remember(fd, "/");
#endif
  for (const auto& part : path.relative_path()) {
    if (part.empty() || part == "." || part == "..") { ::close(fd); errno = EINVAL; return -1; }
    if (fd < 0) return -1;
    const int child = OpenWalkChild(fd, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    const int error = errno;
    ::close(fd);
    fd = child;
    errno = error;
  }
  return fd;
}
}  // namespace playden::platform
