// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Opens a copy plan's files from the game folder, one walk step at a time (platform/directory_walk.h):
// the folders of the current file stay open, so files in the same folder open in one step. Shared by
// the imported games' copy (copy_runner.cpp) and the installed games' move (installs/game_move.cpp).

#include "platform/directory_walk.h"

#include <cstdint>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace playden::stores::local {

class SourceWalk {
 public:
  explicit SourceWalk(std::string source) : source_(std::move(source)) {}
  ~SourceWalk() {
    for (auto& level : chain_) ::close(level.fd);
    if (root_.fd >= 0) ::close(root_.fd);
  }
  SourceWalk(const SourceWalk&) = delete;
  SourceWalk& operator=(const SourceWalk&) = delete;

  // A descriptor for the file (relative, '/' separators), or -1 with errno.
  int OpenFile(const std::string& relative) {
    if (root_.fd < 0) {
      root_.fd = platform::OpenDirectoryWalk(source_);
      if (root_.fd < 0) return -1;
      root_.path = source_;
    }
    const auto parts = Split(relative);
    std::size_t common = 0;
    while (common < chain_.size() && common + 1 < parts.size() && chain_[common].name == parts[common]) ++common;
    while (chain_.size() > common) {
      ::close(chain_.back().fd);
      chain_.pop_back();
    }
    for (std::size_t index = common; index + 1 < parts.size(); ++index) {
      Level& parent = chain_.empty() ? root_ : chain_.back();
      const int fd = platform::OpenWalkChild(parent.fd, parts[index].c_str(), kOpenFolder, Listed(parent, parts[index]));
      if (fd < 0) return -1;
      chain_.push_back(Level{parts[index], parent.path + "/" + parts[index], fd, {}, false});
    }
    Level& folder = chain_.empty() ? root_ : chain_.back();
    return platform::OpenWalkChildUnprivileged(folder.fd, parts.back().c_str(), kOpenFile, Listed(folder, parts.back()));
  }

 private:
  static constexpr int kOpenFolder = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
  static constexpr int kOpenFile = O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;
  struct Level {
    std::string name;
    std::string path;  // the folder this level holds, for a listing that goes by path
    int fd = -1;
    std::unordered_map<std::string, std::uint64_t> listed;
    bool listed_read = false;
  };
  static std::vector<std::string> Split(const std::string& path) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
      const auto slash = path.find('/', start);
      parts.push_back(path.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
      if (slash == std::string::npos) return parts;
      start = slash + 1;
    }
  }
  // A walk by path checks each step against its folder's listing; the listing is read once per
  // folder instead of once per file (0: the walk reads it itself).
  std::uint64_t Listed(Level& level, const std::string& name) {
#if defined(PLAYDEN_WALK_BY_PATH)
    if (!level.listed_read) {
      level.listed_read = true;
      DIR* listing = platform::OpenHeldListing(level.fd, level.path);
      if (listing) {
        while (const auto* entry = ::readdir(listing)) level.listed[entry->d_name] = entry->d_fileno;
        ::closedir(listing);
      }
    }
    const auto found = level.listed.find(name);
    return found == level.listed.end() ? 0 : found->second;
#else
    (void)level;
    (void)name;
    return 0;
#endif
  }

  std::string source_;
  Level root_;
  std::vector<Level> chain_;
};

}  // namespace playden::stores::local
