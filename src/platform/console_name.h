// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef PLAYDEN_PLATFORM_CONSOLE_NAME_H
#define PLAYDEN_PLATFORM_CONSOLE_NAME_H

// The console's name for the player, detected from the toolchain rather than configured: Sony
// defines __ORBIS__ for the PS4 and __PROSPERO__ for the PS5.
//
// ⚠ __ORBIS__ MUST BE TESTED FIRST. The PS4 build's compiler wrapper
// (Orbis-Win build/ps4-wine-sdk/bin/prospero-clang) deliberately defines __PROSPERO__ as well, so
// that prospero-win's PS5 code compiles for the PS4; testing __PROSPERO__ first would make every
// PS4 call itself a PS5. PLAYDEN_PLATFORM_PS5 cannot be used here either -- it means "a console
// rather than the desktop host", and the PS4 build defines it too.
//
// This names the machine in text the player reads. It is not a capability or a protocol value:
// the "ps5" audio driver and a recipe's audio=ps5 requirement keep their names on every console.
#if defined(__ORBIS__)
#define PLAYDEN_CONSOLE_NAME "PS4"
#elif defined(__PROSPERO__)
#define PLAYDEN_CONSOLE_NAME "PS5"
#else
// Desktop host builds and tests describe the console they stand in for.
#define PLAYDEN_CONSOLE_NAME "PS5"
#endif

#endif
