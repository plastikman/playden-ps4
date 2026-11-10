// SPDX-License-Identifier: LGPL-2.1-or-later
// Playden's PS4 UI heap; see playden_heap.c.
#ifndef PLAYDEN_PS4_HEAP_H
#define PLAYDEN_PS4_HEAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PlaydenHeapStats {
    uint64_t live_bytes, peak_live_bytes, mapped_bytes, peak_mapped_bytes;
    uint64_t allocations, frees, failures, foreign_frees, foreign_reallocs;
    uint64_t last_failed_bytes, last_map_bytes, last_map_errno;
} PlaydenHeapStats;

void playden_heap_stats(PlaydenHeapStats *stats);

#ifdef __cplusplus
}
#endif

#endif
