// time_text.hpp - The clock and dates the UI shows, without strftime.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdio>
#include <ctime>
#include <string>

namespace playden::homebrew
{

// strftime reads the C library's locale for every call, and the PS4 port's UI module has no locale
// set up for its threads: the call faults there. These give strftime's C-locale text from the fields.

// "21:42": strftime's "%H:%M".
inline std::string ClockText(const std::tm &time)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%02d:%02d", time.tm_hour, time.tm_min);
    return text;
}

// "07 Nov 14:05", or "07 Nov 2024" with the year: strftime's "%d %b %H:%M" and "%d %b %Y".
inline std::string DayText(const std::tm &time, bool with_year)
{
    static constexpr const char *kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const char *month = time.tm_mon >= 0 && time.tm_mon < 12 ? kMonths[time.tm_mon] : "?";
    char text[32];
    if (with_year) std::snprintf(text, sizeof(text), "%02d %s %d", time.tm_mday, month, time.tm_year + 1900);
    else std::snprintf(text, sizeof(text), "%02d %s %02d:%02d", time.tm_mday, month, time.tm_hour, time.tm_min);
    return text;
}

} // namespace playden::homebrew
