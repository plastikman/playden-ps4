// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <orbis/libkernel.h>
#include <stdint.h>
#include <stdio.h>

enum { EXPORT_COUNT = 1 };
#define PRX_MAGIC UINT64_C(0x3143534544585250)

typedef struct { const char *name; const void *address; } Export;
typedef struct { uint64_t magic; uint32_t version, count; Export exports[EXPORT_COUNT]; } Descriptor;

extern int playden_ps4_main(int argc, char **argv);

/* The module's static constructors. The stub starts this PRX with sceKernelLoadStartModule, which
 * runs a module's init only through Sony's C runtime; an OpenOrbis-linked module has none, so its
 * .init_array (DT_INIT_ARRAY) was never walked and every namespace-scope object with a constructor
 * stayed all-zero (a std::map crashed 01.91). lld defines these bounds for the shared object once
 * they are referenced; the entries are relocated with the rest of the module. */
typedef void (*InitFunction)(void);
extern InitFunction __init_array_start[] __attribute__((weak, visibility("hidden")));
extern InitFunction __init_array_end[] __attribute__((weak, visibility("hidden")));

/* Set by a constructor of its own, so the log shows the table really ran. volatile, or the compiler
 * folds the constructor into the variable's initial value and drops it (LLVM's GlobalOpt did). */
static volatile int initializers_reached;
__attribute__((constructor)) static void note_initializers_reached(void) { initializers_reached = 1; }

static void run_static_initializers(void)
{
    char line[96];
    unsigned count = 0;

    for (InitFunction *entry = __init_array_start; entry && entry < __init_array_end; entry++) {
        if (*entry && *entry != (InitFunction)(intptr_t)-1) (*entry)();
        count++;
    }
    snprintf(line, sizeof(line), "PLDN10001: static initializers ran=%u sentinel=%d\n", count,
             initializers_reached);
    sceKernelDebugOutText(0, line);
}

__attribute__((visibility("default"), noreturn))
void playden_ps4_start(int argc, char **argv)
{
    run_static_initializers();
    (void)playden_ps4_main(argc, argv);
    for (;;) sceKernelUsleep(1000000);
}

__attribute__((used, visibility("default"), section(".rodata"), aligned(16)))
const Descriptor playden_ps4_descriptor = {
    PRX_MAGIC, 1, EXPORT_COUNT,
    {{ "playden_ps4_start", (const void *)playden_ps4_start }}
};
