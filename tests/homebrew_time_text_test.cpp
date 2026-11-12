// homebrew_time_text_test.cpp - The UI's clock and dates match strftime in the C locale.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include "homebrew/time_text.hpp"

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

using namespace playden::homebrew;

namespace {

std::string Strftime(const char *format, const std::tm &time)
{
    char text[64]{};
    return std::strftime(text, sizeof(text), format, &time) ? text : "";
}

} // namespace

int main()
{
    std::setlocale(LC_ALL, "C");
    std::tm time{};
    time.tm_year = 124; // 2024
    for (int month = 0; month < 12; ++month)
        for (const int day : {1, 9, 10, 31})
            for (const int hour : {0, 9, 23})
                for (const int minute : {0, 5, 59}) {
                    time.tm_mon = month;
                    time.tm_mday = day;
                    time.tm_hour = hour;
                    time.tm_min = minute;
                    CHECK(ClockText(time) == Strftime("%H:%M", time));
                    CHECK(DayText(time, false) == Strftime("%d %b %H:%M", time));
                    CHECK(DayText(time, true) == Strftime("%d %b %Y", time));
                }
    CHECK(DayText(time, true) == "31 Dec 2024");
    std::puts("homebrew time text passed");
    return 0;
}
