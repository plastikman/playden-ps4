#pragma once

#include "runtime/install_service.h"
#include "runtime/game_install.h"

#include <filesystem>

namespace playden::ps4 {
// Preserve mutable files from the pre-kit Playden prefixes. The original is
// retained so a failed or interrupted migration can be retried safely.
runtime::PackageActionResult MigrateLegacyBottle(const runtime::RuntimeInstall &record,
                                                   const std::filesystem::path &bottle);
}
