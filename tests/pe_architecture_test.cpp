// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/pe_architecture.h"

#include <array>
#include <cstdlib>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <unistd.h>

void Check(bool okay, const char* message) {
  if (okay) return;
  std::fprintf(stderr, "%s\n", message);
  std::exit(1);
}

int main() {
  const std::string path = "pe_architecture_test.exe";
  std::array<unsigned char, 128> pe{};
  pe[0] = 'M'; pe[1] = 'Z'; pe[0x3c] = 64;
  pe[64] = 'P'; pe[65] = 'E';
  const int file = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
  Check(file >= 0, "could not create PE fixture");
  const auto check = [&](unsigned char machine_lo, unsigned char machine_hi,
                         unsigned char magic_lo, unsigned char magic_hi,
                         const char* expected) {
    pe[68] = machine_lo; pe[69] = machine_hi;
    pe[88] = magic_lo; pe[89] = magic_hi;
    Check(::pwrite(file, pe.data(), pe.size(), 0) == static_cast<ssize_t>(pe.size()),
          "could not write PE fixture");
    Check(playden::runtime::ExecutableArchitecture(path) == expected, "PE architecture mismatch");
    if (expected[0]) Check(playden::runtime::LaunchArchitecture(path, "32") == expected,
                           "PE architecture must override Steam's hint");
  };
  check(0x4c, 0x01, 0x0b, 0x01, "pe32");
  check(0x64, 0x86, 0x0b, 0x02, "pe64");
  check(0x64, 0x86, 0x0b, 0x01, "");
  Check(playden::runtime::LaunchArchitecture("", "64") == "pe64", "Steam 64-bit hint ignored");
  Check(playden::runtime::LaunchArchitecture("", "32") == "pe32", "Steam 32-bit hint ignored");
  ::close(file);
  std::remove(path.c_str());
}
