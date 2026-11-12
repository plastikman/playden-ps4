// Playden PS4 port - the bundled Orbis-Win as Playden's runtime kit.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "homebrew/runtime_host.hpp"

#include <optional>
#include <string>

namespace playden::ps4 {

// Where the PS4 build describes the bundled Orbis-Win (scripts/ps4-kit-view.py).
inline constexpr const char *kKitRoot = "/app0";
inline constexpr const char *kKitView = "/app0/runtime-template/playden-kit-view.json";

// RuntimeHostConfig::admit_kit for the PS4: the package's kit view, checked for shape and for the
// template it names. Nothing with *reason when the package carries no usable view.
std::optional<playden::homebrew::RuntimeKit> AdmitPs4Kit(std::string *reason);

}  // namespace playden::ps4
