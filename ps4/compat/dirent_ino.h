// The OpenOrbis SDK's dirent.h declares the inode field as d_fileno and then defines
// d_fileno as d_ino, so neither name reaches the field once the header is included.
// Force-included into the PS4 builds of Playden's shared code: name it d_ino -> d_fileno.
#pragma once
#include <dirent.h>
#undef d_fileno
#define d_ino d_fileno
