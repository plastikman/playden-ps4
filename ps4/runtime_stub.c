// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <orbis/libkernel.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { MODULE_INFO_BYTES = 0x160, MAX_SEGMENTS = 4, MAX_EXPORTS = 8 };
#define PRX_MAGIC UINT64_C(0x3143534544585250)

typedef struct { const char *name; const void *address; } Export;
typedef struct { uint64_t magic; uint32_t version, count; Export exports[]; } Descriptor;
typedef struct { const void *address; uint32_t size, protection; } Segment;
typedef void (*Entry)(int, char **);

static void log_line(const char *line)
{
    sceKernelDebugOutText(0, line);
    sceKernelDebugOutText(0, "\n");
}

static int module_segments(int32_t handle, Segment out[MAX_SEGMENTS], uint32_t *count)
{
    uint8_t raw[MODULE_INFO_BYTES] __attribute__((aligned(16)));
    memset(raw, 0, sizeof(raw));
    *(uint64_t *)raw = sizeof(raw);
    if (sceKernelGetModuleInfo((OrbisKernelModule)handle,
                               (OrbisKernelModuleInfo *)raw) < 0) return -1;
    uint32_t number;
    memcpy(&number, raw + 0x148, sizeof(number));
    if (!number || number > MAX_SEGMENTS) return -1;
    for (uint32_t i = 0; i < number; ++i) {
        uint8_t *item = raw + 0x108 + i * 16;
        uint64_t address;
        memcpy(&address, item, sizeof(address));
        memcpy(&out[i].size, item + 8, sizeof(out[i].size));
        memcpy(&out[i].protection, item + 12, sizeof(out[i].protection));
        out[i].address = (const void *)(uintptr_t)address;
        if (!address || !out[i].size) return -1;
    }
    *count = number;
    return 0;
}

static const Descriptor *find_descriptor(const Segment *segments, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) {
        if (!(segments[i].protection & 1u)) continue;
        const uint8_t *base = segments[i].address;
        for (size_t offset = 0; offset + 16 <= segments[i].size; offset += 16) {
            uint64_t magic;
            memcpy(&magic, base + offset, sizeof(magic));
            if (magic != PRX_MAGIC) continue;
            const Descriptor *descriptor = (const Descriptor *)(base + offset);
            if (descriptor->version == 1 && descriptor->count && descriptor->count <= MAX_EXPORTS &&
                offset + 16 + descriptor->count * sizeof(Export) <= segments[i].size)
                return descriptor;
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int result = 0;
    uint32_t handle = sceKernelLoadStartModule(
        "/app0/sce_module/playden_ui.prx", 0, NULL, 0, NULL, &result);
    if (handle & 0x80000000u) {
        log_line("PLDN10001: could not load the Playden UI module");
        for (;;) sceKernelUsleep(1000000);
    }
    Segment segments[MAX_SEGMENTS];
    uint32_t count = 0;
    const Descriptor *descriptor = NULL;
    if (module_segments((int32_t)handle, segments, &count) == 0)
        descriptor = find_descriptor(segments, count);
    Entry entry = NULL;
    if (descriptor) {
        for (uint32_t i = 0; i < descriptor->count; ++i)
            if (!strcmp(descriptor->exports[i].name, "playden_ps4_start"))
                entry = (Entry)descriptor->exports[i].address;
    }
    if (!entry) {
        log_line("PLDN10001: Playden UI entry descriptor missing");
        for (;;) sceKernelUsleep(1000000);
    }
    entry(argc, argv);
    log_line("PLDN10001: Playden UI module returned unexpectedly");
    for (;;) sceKernelUsleep(1000000);
}
