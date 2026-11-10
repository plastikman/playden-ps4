// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <unistd.h>

namespace playden::runtime {

// Return the architecture from the executable itself, or empty when it is not
// a supported PE image. Keep launch profile generation and game settings in
// agreement about the actual program on disk.
inline std::string ExecutableArchitecture(const std::string& path) {
  const int file = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (file < 0) return {};
  const auto read_at = [file](off_t offset, unsigned char* buffer, std::size_t size) {
    std::size_t done = 0;
    while (done < size) {
      const auto count = ::pread(file, buffer + done, size - done, offset + static_cast<off_t>(done));
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) return false;
      done += static_cast<std::size_t>(count);
    }
    return true;
  };
  unsigned char dos[64];
  unsigned char header[26];
  std::string architecture;
  if (read_at(0, dos, sizeof(dos)) && dos[0] == 'M' && dos[1] == 'Z') {
    const std::uint32_t pe = dos[0x3c] | (dos[0x3d] << 8) | (dos[0x3e] << 16) |
                             (static_cast<std::uint32_t>(dos[0x3f]) << 24);
    if (pe < 64u * 1024u * 1024u && read_at(static_cast<off_t>(pe), header, sizeof(header)) &&
        header[0] == 'P' && header[1] == 'E' && header[2] == 0 && header[3] == 0) {
      const unsigned machine = header[4] | (header[5] << 8);
      const unsigned magic = header[24] | (header[25] << 8);
      if (machine == 0x014c && magic == 0x010b) architecture = "pe32";
      if (machine == 0x8664 && magic == 0x020b) architecture = "pe64";
    }
  }
  ::close(file);
  return architecture;
}

inline std::string LaunchArchitecture(const std::string& installed_executable,
                                      std::string_view steam_os_arch) {
  const auto detected = installed_executable.empty() ? std::string()
      : ExecutableArchitecture(installed_executable);
  if (!detected.empty()) return detected;
  return steam_os_arch == "64" ? "pe64" : "pe32";
}

}  // namespace playden::runtime
