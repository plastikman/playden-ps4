// drop_folder_access.cpp - Opens the files of a game copied over FTP into the drop folder.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include "stores/local/drop_folder_access.h"
#include "platform/directory_walk.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include "platform/console_name.h"

namespace playden::stores::local {
namespace {
namespace fs = std::filesystem;

constexpr int kMaxDepth = 64;
constexpr std::size_t kMaxEntries = 1000000;
constexpr int kOpenFlags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;

std::string Error(int error) { return std::to_string(error) + " (" + std::strerror(error) + ")"; }
bool Unsupported(int error) { return error == ENOSYS || error == EINVAL; }
bool IsLink(int error) { return error == ELOOP || error == EMLINK; }  // EMLINK: FreeBSD's O_NOFOLLOW

bool PlainName(const std::string& name) {
  return !name.empty() && name != "." && name != ".." && name.find('/') == std::string::npos &&
         name.find('\0') == std::string::npos;
}

struct Child {
  std::string name;
  std::uint64_t inode = 0;
  unsigned char type = DT_UNKNOWN;
};
struct Frame {
  int fd = -1;
  fs::path relative;  // to the entry; empty for the entry itself
  std::vector<Child> children;
  std::size_t next = 0;
  int depth = 0;
};
// A root-owned entry whose mode the next raise changes (the descriptor is the item's).
struct Pending {
  int fd = -1;
  mode_t mode = 0;
  fs::path relative;
};

class Walk {
 public:
  Walk(std::string entry, const DropFolderAccessOptions& options)
      : entry_(std::move(entry)), options_(options), app_(options.app_uid ? *options.app_uid : ::geteuid()) {}
  ~Walk() {
    for (auto& frame : stack_) if (frame.fd >= 0) ::close(frame.fd);
    for (auto& item : pending_) ::close(item.fd);
  }

  DropFolderAccessResult Run() {
    const fs::path console = entry_;
    if (!OpenEntry(console.filename().string())) return Finish();
    while (!stack_.empty() && message_.empty()) {
      Frame& top = stack_.back();
      if (top.next == top.children.size()) {
        ::close(top.fd);
        stack_.pop_back();
        continue;
      }
      const Child child = top.children[top.next++];
      Visit(top, child);
      if (pending_.size() >= std::max<std::size_t>(options_.batch, 1)) Flush();
    }
    if (message_.empty()) Flush();
    result_.ok = message_.empty();
    return Finish();
  }

 private:
  std::string Console(const fs::path& relative) const {
    return relative.empty() ? entry_ : entry_ + "/" + relative.generic_string();
  }
  fs::path Host(const std::string& console) const { return options_.namespace_root / fs::path(console).relative_path(); }
  void Note(const std::string& line) const { if (options_.log) options_.log(line); }

  bool Fail(const fs::path& relative, const std::string& detail, const std::string& log_detail) {
    if (!message_.empty()) return false;
    message_ = "Playden can't open the game's files in " + entry_ + ": " + detail +
               ". Fix it over FTP (folders 777, files 666), then try again.";
    Note("drop folder access failed entry=" + entry_ + " path=" + Console(relative) + " " + log_detail);
    return false;
  }
  bool Refuse(const fs::path& relative, int error) {
    return Fail(relative, "this console can't walk the folder safely", "unsupported=" + Error(error));
  }

  bool AsRoot(const std::function<bool()>& work) {
    ++result_.raises;
    return options_.as_root && options_.as_root(work);
  }

  // Opens `name` in `parent` as the app user, or in a raise when the app user may not (its mode is
  // queued like any other). -1 with message_ set, or -1 with errno for a link or a gone entry.
  int Open(int parent, const fs::path& relative, const std::string& name, std::uint64_t inode, int flags) {
    int fd = platform::OpenWalkChild(parent, name.c_str(), flags, inode);
    if (fd >= 0) return fd;
    int error = errno;
    if (Unsupported(error)) { Refuse(relative, error); return -1; }
    if (error != EACCES && error != EPERM) { errno = error; return -1; }
    bool opened = false;
    const bool ran = AsRoot([&] {
      fd = platform::OpenWalkChild(parent, name.c_str(), flags, inode);
      error = errno;
      opened = fd >= 0;
      return opened;
    });
    // An fd opened while root could not be dropped again is not used.
    if (!ran && fd >= 0) { ::close(fd); fd = -1; }
    if (opened && ran) return fd;
    if (Unsupported(error)) { Refuse(relative, error); return -1; }
    if (IsLink(error) || error == ENOENT) { errno = error; return -1; }
    Fail(relative, Console(relative) + " can't be opened", "open=" + Error(error) + (options_.as_root ? "" : " root=unavailable"));
    return -1;
  }

  // Wants `mode` on fd (owned by this call when queued for root).
  bool Change(int fd, const struct stat& info, mode_t mode, const fs::path& relative, bool keep_fd) {
    if ((info.st_mode & 07777) == mode) return true;
    ++result_.changed;
    if (info.st_uid == app_) {
      if (::fchmod(fd, mode) == 0) return true;
      return Fail(relative, Console(relative) + " can't be changed", "chmod=" + Error(errno));
    }
    if (keep_fd) {
      // PS4 titles cannot duplicate a directory descriptor. Change the held folder before
      // listing it; files still use the short batched root raises below.
      int error = 0;
      const bool changed = AsRoot([&] {
        if (::fchmod(fd, mode) == 0) return true;
        error = errno;
        return false;
      });
      if (changed) return true;
      return Fail(relative, Console(relative) + " can't be changed",
                  error ? "chmod=" + Error(error) : options_.as_root ? "root=refused" : "root=unavailable");
    }
    pending_.push_back({fd, mode, relative});
    return true;
  }

  // One raise for the queued root-owned entries.
  void Flush() {
    if (pending_.empty()) return;
    auto items = std::move(pending_);
    pending_.clear();
    std::size_t done = 0;
    int error = 0;
    const bool ran = AsRoot([&] {
      for (; done < items.size(); ++done)
        if (::fchmod(items[done].fd, items[done].mode) != 0) { error = errno; return false; }
      return true;
    });
    if (!ran && message_.empty()) {
      const auto& failed = items[std::min(done, items.size() - 1)];
      Fail(failed.relative, Console(failed.relative) + " can't be changed",
           error ? "chmod=" + Error(error) : options_.as_root ? "root=refused" : "root=unavailable");
    }
    for (auto& item : items) ::close(item.fd);
  }

  // Takes fd: checks the folder, queues its mode and lists it onto the stack.
  bool EnterFolder(int fd, const fs::path& relative, int depth) {
    struct stat info{};
    if (::fstat(fd, &info) != 0) { const int e = errno; ::close(fd); return Fail(relative, Console(relative) + " can't be read", "fstat=" + Error(e)); }
    if (!S_ISDIR(info.st_mode)) { ::close(fd); return Fail(relative, Console(relative) + " is not a folder", "type=not-folder"); }
    if (depth > kMaxDepth) { ::close(fd); return Fail(relative, "its folders go too deep", "depth=" + std::to_string(depth)); }
    ++result_.folders;
    if (!Change(fd, info, 0777, relative, true)) { ::close(fd); return false; }
    Frame frame;
    frame.fd = fd;
    frame.relative = relative;
    frame.depth = depth;
    DIR* listing = platform::OpenHeldListing(fd, Host(Console(relative)));
    if (!listing) {
      const int e = errno;
      ::close(fd);
      return Fail(relative, Console(relative) + " can't be listed", "list=" + Error(e));
    }
    int error = 0;
    for (;;) {
      errno = 0;
      const auto* entry = ::readdir(listing);
      if (!entry) { error = errno; break; }
      const std::string name = entry->d_name;
      if (name == "." || name == "..") continue;
      if (++entries_ > kMaxEntries) { error = EFBIG; break; }
      frame.children.push_back({name, static_cast<std::uint64_t>(entry->d_fileno), entry->d_type});
    }
    ::closedir(listing);
    if (error) {
      ::close(fd);
      return error == EFBIG ? Fail(relative, "it has too many files", "entries>" + std::to_string(kMaxEntries))
                            : Fail(relative, Console(relative) + " can't be listed", "readdir=" + Error(error));
    }
    stack_.push_back(std::move(frame));
    return true;
  }

  bool OpenEntry(const std::string& name) {
    const int parent = platform::OpenDirectoryWalk(Host(kDropFolder));
    if (parent < 0) {
      const int e = errno;
      if (Unsupported(e)) return Refuse({}, e);
      return Fail({}, std::string(kDropFolder) + " can't be opened", "open-parent=" + Error(e));
    }
    const int fd = Open(parent, {}, name, 0, kOpenFlags | O_DIRECTORY);
    const int error = errno;
    ::close(parent);
    if (fd < 0) {
      if (!message_.empty()) return false;
      if (IsLink(error) || error == ENOTDIR) return Fail({}, "the game's folder is a link or a file", "open=" + Error(error));
      return Fail({}, "the game's folder is missing", "open=" + Error(error));
    }
    return EnterFolder(fd, {}, 0);
  }

  // A link stays; one that leads outside the entry fails the walk.
  void CheckLink(const Frame& folder, const std::string& name) {
    ++result_.links;
    const auto relative = folder.relative / name;
#if defined(PLAYDEN_WALK_BY_PATH)
    // A title has neither readlinkat with a descriptor nor readlink (a libkernel_sys-only import,
    // null in the title): where the link leads can't be checked, so it fails like one that leaves.
    (void)folder;
    Fail(relative, "the link " + Console(relative) + " can't be checked on the " PLAYDEN_CONSOLE_NAME " (remove it over FTP)",
         "link=unchecked");
#else
    char target[4096];
    const auto length = ::readlinkat(folder.fd, name.c_str(), target, sizeof(target) - 1);
    if (length < 0) {
      const int e = errno;
      if (e == ENOENT) return;
      Fail(relative, "the link " + Console(relative) + " can't be read", "readlink=" + Error(e));
      return;
    }
    target[length] = '\0';
    const fs::path to(target);
    bool inside;
    if (to.is_absolute()) {
      const auto within = to.lexically_normal().lexically_relative(entry_);
      inside = !within.empty() && *within.begin() != "..";
    } else {
      const auto resolved = (folder.relative / to).lexically_normal();
      inside = resolved.empty() || (!resolved.is_absolute() && *resolved.begin() != "..");
    }
    if (!inside)
      Fail(relative, "the link " + Console(relative) + " leads outside the game's folder", "link-target=" + std::string(target));
#endif
  }

  void Visit(Frame& folder, const Child& child) {
    const auto relative = folder.relative / child.name;
    if (child.type == DT_LNK) return CheckLink(folder, child.name);
    if (child.type != DT_REG && child.type != DT_DIR && child.type != DT_UNKNOWN) return;  // never opened
    const int fd = Open(folder.fd, relative, child.name, child.inode, kOpenFlags);
    if (fd < 0) {
      if (!message_.empty()) return;
      const int error = errno;
      if (IsLink(error)) return CheckLink(folder, child.name);  // a listing that said DT_UNKNOWN
      if (error == ENOENT) return;  // removed meanwhile
      Fail(relative, Console(relative) + " can't be opened", "open=" + Error(error));
      return;
    }
    struct stat info{};
    if (::fstat(fd, &info) != 0) {
      const int e = errno;
      ::close(fd);
      Fail(relative, Console(relative) + " can't be read", "fstat=" + Error(e));
      return;
    }
    if (S_ISDIR(info.st_mode)) {
      // `folder` is invalid once a frame is pushed.
      EnterFolder(fd, relative, folder.depth + 1);
      return;
    }
    if (!S_ISREG(info.st_mode)) { ::close(fd); return; }
    ++result_.files;
    if (info.st_nlink != 1) {
      // Another name has the same content: left alone, as the shared profile does.
      ++result_.hard_linked;
      Note("drop folder access skips " + Console(relative) + ": it has " + std::to_string(info.st_nlink) + " links");
      ::close(fd);
      return;
    }
    const mode_t wanted = 0666 | (info.st_mode & 0111);
    const bool queued = (info.st_mode & 07777) != wanted && info.st_uid != app_;
    if (!Change(fd, info, wanted, relative, false) || !queued) ::close(fd);
  }

  DropFolderAccessResult Finish() {
    result_.message = message_;
    if (message_.empty())
      Note("drop folder opened entry=" + entry_ + " folders=" + std::to_string(result_.folders) +
           " files=" + std::to_string(result_.files) + " changed=" + std::to_string(result_.changed) +
           " hard-linked=" + std::to_string(result_.hard_linked) + " links=" + std::to_string(result_.links) +
           " raises=" + std::to_string(result_.raises));
    return result_;
  }

  const std::string entry_;
  const DropFolderAccessOptions& options_;
  const uid_t app_;
  std::vector<Frame> stack_;
  std::vector<Pending> pending_;
  std::size_t entries_ = 0;
  std::string message_;
  DropFolderAccessResult result_;
};

// Removes `name` from the held folder `parent` (a folder when `folder`) without following it.
int RemoveWalkChild(int parent, const char* name, bool folder) {
  if (!PlainName(name ? name : "")) { errno = EINVAL; return -1; }
#if defined(PLAYDEN_WALK_BY_PATH)
  // A title has no unlinkat with a real descriptor: the removal goes by path once the path still
  // names the held parent (as MakeWalkChild checks), and unlink/rmdir never follow the last name.
  fs::path parent_path;
  if (!platform::walk_detail::PathOf(parent, &parent_path)) return -1;
  struct stat held{}, named{};
  if (::fstat(parent, &held) != 0 || ::lstat(parent_path.c_str(), &named) != 0) return -1;
  if (!S_ISDIR(named.st_mode) || named.st_dev != held.st_dev || named.st_ino != held.st_ino) { errno = ESTALE; return -1; }
  const auto path = parent_path / name;
  return folder ? ::rmdir(path.c_str()) : ::unlink(path.c_str());
#else
  return ::unlinkat(parent, name, folder ? AT_REMOVEDIR : 0);
#endif
}

// Stats `name` in the held folder `parent` without following it.
int StatWalkChild(int parent, const char* name, struct stat* info) {
  if (!PlainName(name ? name : "")) { errno = EINVAL; return -1; }
#if defined(PLAYDEN_WALK_BY_PATH)
  // By path, as RemoveWalkChild: once the path still names the held parent; lstat never follows.
  fs::path parent_path;
  if (!platform::walk_detail::PathOf(parent, &parent_path)) return -1;
  struct stat held{}, named{};
  if (::fstat(parent, &held) != 0 || ::lstat(parent_path.c_str(), &named) != 0) return -1;
  if (!S_ISDIR(named.st_mode) || named.st_dev != held.st_dev || named.st_ino != held.st_ino) { errno = ESTALE; return -1; }
  return ::lstat((parent_path / name).c_str(), info);
#else
  return ::fstatat(parent, name, info, AT_SYMLINK_NOFOLLOW);
#endif
}

class Removal {
 public:
  Removal(const DropFolderPlanned& planned, const runtime::WritableFoldersAsRoot& as_root)
      : planned_(planned), as_root_(as_root) {}
  ~Removal() {
    for (auto& frame : stack_) if (frame.fd >= 0) ::close(frame.fd);
  }

  DropFolderRemoval Run(const std::string& drop_folder, const std::string& name) {
    root_ = fs::path(drop_folder) / name;
    const int parent = platform::OpenDirectoryWalk(drop_folder);
    if (parent < 0) return Done("open-parent=" + Error(errno));
    const int fd = Open(parent, name);
    if (fd < 0) {
      const int error = errno;
      ::close(parent);
      return Done("open=" + Error(error));
    }
    if (!Enter(fd, {}, 0)) { ::close(parent); return Done(message_); }
    while (!stack_.empty() && message_.empty()) {
      Frame& top = stack_.back();
      if (top.next == top.children.size()) {
        // Its children are done: the folder goes from its parent (the drop folder for the entry).
        const auto relative = top.relative;
        ::close(top.fd);
        stack_.pop_back();
        const int holder = stack_.empty() ? parent : stack_.back().fd;
        const auto leaf = relative.empty() ? name : relative.filename().string();
        if (Remove(holder, leaf, true)) ++result_.folders;
        else if (errno == ENOTEMPTY || errno == EEXIST) ++result_.kept;  // something it kept is inside
        else Fail(relative, "rmdir=" + Error(errno));
        continue;
      }
      const Child child = top.children[top.next++];
      Visit(top, child);
    }
    ::close(parent);
    if (message_.empty() && result_.kept > 0) message_ = "kept=" + std::to_string(result_.kept);
    return Done(message_);
  }

 private:
  bool AsRoot(const std::function<bool()>& work) {
    ++result_.raises;
    return as_root_ && as_root_(work);
  }
  int Open(int parent, const std::string& name, std::uint64_t inode = 0) {
    const int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    int fd = platform::OpenWalkChild(parent, name.c_str(), flags, inode);
    if (fd >= 0 || (errno != EACCES && errno != EPERM)) return fd;
    int error = 0;
    const bool ran = AsRoot([&] {
      fd = platform::OpenWalkChild(parent, name.c_str(), flags, inode);
      error = errno;
      return fd >= 0;
    });
    if (!ran && fd >= 0) { ::close(fd); fd = -1; }
    errno = fd >= 0 ? 0 : error ? error : EACCES;
    return fd;
  }
  // As the app user, then in a raise when the folder holding it isn't the app user's to change.
  bool Remove(int parent, const std::string& name, bool folder) {
    if (RemoveWalkChild(parent, name.c_str(), folder) == 0) return true;
    if (errno != EACCES && errno != EPERM) return false;
    int error = 0;
    const bool removed = AsRoot([&] {
      if (RemoveWalkChild(parent, name.c_str(), folder) == 0) return true;
      error = errno;
      return false;
    });
    errno = removed ? 0 : error ? error : EACCES;
    return removed;
  }
  bool Enter(int fd, const fs::path& relative, int depth) {
    if (depth > kMaxDepth) { ::close(fd); return Fail(relative, "depth=" + std::to_string(depth)); }
    Frame frame;
    frame.fd = fd;
    frame.relative = relative;
    frame.depth = depth;
    DIR* listing = platform::OpenHeldListing(fd, root_ / relative);
    if (!listing) {
      const int e = errno;
      ::close(fd);
      return Fail(relative, "list=" + Error(e));
    }
    int error = 0;
    for (;;) {
      errno = 0;
      const auto* entry = ::readdir(listing);
      if (!entry) { error = errno; break; }
      const std::string child = entry->d_name;
      if (child == "." || child == "..") continue;
      if (++entries_ > kMaxEntries) { ::closedir(listing); ::close(fd); return Fail(relative, "entries>" + std::to_string(kMaxEntries)); }
      frame.children.push_back({child, static_cast<std::uint64_t>(entry->d_fileno), entry->d_type});
    }
    ::closedir(listing);
    if (error) { ::close(fd); return Fail(relative, "readdir=" + Error(error)); }
    stack_.push_back(std::move(frame));
    return true;
  }
  void Visit(Frame& folder, const Child& child) {
    const auto relative = folder.relative / child.name;
    if (child.type == DT_LNK) { ++result_.kept; return; }  // never followed, never deleted
    if (child.type == DT_DIR || child.type == DT_UNKNOWN) {
      const int fd = Open(folder.fd, child.name, child.inode);
      if (fd >= 0) {
        Enter(fd, relative, folder.depth + 1);  // `folder` is invalid once a frame is pushed
        return;
      }
      const int error = errno;
      if (error == ENOENT) return;                     // gone meanwhile
      if (IsLink(error)) { ++result_.kept; return; }  // a link in its place: left alone
      if (child.type == DT_DIR || error != ENOTDIR) { Fail(relative, "open=" + Error(error)); return; }
    }
    if (child.type != DT_REG && child.type != DT_UNKNOWN) { ++result_.kept; return; }
    // A file: only one the copy took goes, and only as the copy took it.
    struct stat info{};
    if (StatWalkChild(folder.fd, child.name.c_str(), &info) != 0) {
      if (errno != ENOENT) ++result_.kept;  // can't be checked: left alone
      return;
    }
    if (!S_ISREG(info.st_mode) || !planned_ || !planned_(relative.generic_string(), info)) { ++result_.kept; return; }
    if (Remove(folder.fd, child.name, false)) ++result_.files;
    else if (errno != ENOENT) Fail(relative, "unlink=" + Error(errno));
  }
  bool Fail(const fs::path& relative, const std::string& detail) {
    if (message_.empty()) message_ = "path=" + (relative.empty() ? std::string(".") : relative.generic_string()) + " " + detail;
    return false;
  }
  DropFolderRemoval Done(const std::string& message) {
    result_.message = message;
    result_.ok = message.empty();
    return result_;
  }

  const DropFolderPlanned& planned_;
  const runtime::WritableFoldersAsRoot& as_root_;
  fs::path root_;
  std::vector<Frame> stack_;
  std::size_t entries_ = 0;
  std::string message_;
  DropFolderRemoval result_;
};

}  // namespace

DropFolderRemoval RemoveDropFolderEntry(const std::string& drop_folder, const std::string& name,
                                        const DropFolderPlanned& planned,
                                        const runtime::WritableFoldersAsRoot& as_root,
                                        const std::function<void(const std::string&)>& log) {
  DropFolderRemoval result;
  const fs::path folder(drop_folder);
  if (!PlainName(name) || !folder.is_absolute() || folder.lexically_normal() != folder || folder.filename() != "playden-import") {
    result.message = "refused";
  } else {
    result = Removal(planned, as_root).Run(drop_folder, name);
  }
  if (log)
    log("drop folder removal entry=" + drop_folder + "/" + name + (result.ok ? " done" : " incomplete " + result.message) +
        " files=" + std::to_string(result.files) + " folders=" + std::to_string(result.folders) +
        " kept=" + std::to_string(result.kept) + " raises=" + std::to_string(result.raises));
  return result;
}

bool IsDropFolderEntry(const std::string& console_path) {
  const fs::path path(console_path);
  if (console_path.find('\0') != std::string::npos || path.lexically_normal().string() != console_path) return false;
  return path.parent_path() == kDropFolder && PlainName(path.filename().string());
}

DropFolderAccessResult OpenDropFolderEntry(const std::string& entry, const DropFolderAccessOptions& options) {
  if (!IsDropFolderEntry(entry) || !options.namespace_root.is_absolute()) {
    DropFolderAccessResult refused;
    refused.message = "Playden only opens game folders directly inside " + std::string(kDropFolder) + ", not " + entry + ".";
    if (options.log) options.log("drop folder access refused entry=" + entry);
    return refused;
  }
  return Walk(entry, options).Run();
}

DropFolderOpener MakeDropFolderOpener(DropFolderAccessOptions options) {
  return [options = std::move(options)](const std::string& entry) { return OpenDropFolderEntry(entry, options).message; };
}

}  // namespace playden::stores::local
