#pragma once

namespace playden::runtime {

// A managed title's files live beside the host application's data on each platform.
inline constexpr const char* kManagedTitleRoot =
#if defined(PLAYDEN_PLATFORM_PS4) && PLAYDEN_PLATFORM_PS4
    "/data/PLDN10001";
#else
    "/data/homebrew";
#endif

}  // namespace playden::runtime
