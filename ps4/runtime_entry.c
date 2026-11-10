// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <orbis/libkernel.h>
#include <stdint.h>

enum { EXPORT_COUNT = 1 };
#define PRX_MAGIC UINT64_C(0x3143534544585250)

typedef struct { const char *name; const void *address; } Export;
typedef struct { uint64_t magic; uint32_t version, count; Export exports[EXPORT_COUNT]; } Descriptor;

extern int playden_ps4_main(int argc, char **argv);

__attribute__((visibility("default"), noreturn))
void playden_ps4_start(int argc, char **argv)
{
    (void)playden_ps4_main(argc, argv);
    for (;;) sceKernelUsleep(1000000);
}

__attribute__((used, visibility("default"), section(".rodata"), aligned(16)))
const Descriptor playden_ps4_descriptor = {
    PRX_MAGIC, 1, EXPORT_COUNT,
    {{ "playden_ps4_start", (const void *)playden_ps4_start }}
};
