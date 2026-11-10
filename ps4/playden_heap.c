// SPDX-License-Identifier: LGPL-2.1-or-later
// Adapted from Orbis-Win's wine/ps5/pw_wine_heap.c (prospero-win), LGPL-2.1-or-later.
//
// The heap for Playden's UI module on PS4.
//
// The title's libc heap (libSceLibcInternal) stops at about 13 MiB and does not grow,
// whatever the eboot's sceLibcHeapSize/sceLibcHeapExtendedAlloc ask for (measured on
// 01.85: system=13712 KiB before and after malloc refused, flexible memory untouched).
// Playden idles near 8 MiB, so one Steam depot manifest or a second download worker's
// chunk buffers was enough to exhaust it. Orbis-Win hit the same ceiling in wineserver
// and moved Wine's Unix side to its own heap; this is that heap for Playden.
//
// The UI PRX is linked with -Wl,--wrap for malloc, calloc, realloc, free and
// posix_memalign, so every allocation made by Playden, libc++, curl and mbedtls comes
// here. Small blocks (up to 64 KiB) come from power-of-two size classes carved out of
// 1 MiB anonymous mappings; larger blocks get their own mapping and are unmapped on
// free. Anonymous mappings are flexible memory, of which the title has about 430 MiB
// while the UI runs; the game itself runs after LoadExec in a fresh process.
//
// A pointer this heap did not return (strdup, vasprintf and realpath results, which
// libc allocates internally) is passed to libc's own free/realloc.
#include "playden_heap.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif

void *__real_malloc(size_t bytes);
void *__real_realloc(void *pointer, size_t bytes);
void __real_free(void *pointer);

enum {
    HEADER = 16, MIN_SHIFT = 4, MAX_SHIFT = 16, CLASSES = MAX_SHIFT - MIN_SHIFT + 1,
    SPAN_BYTES = 1u << 20, MAX_SPANS = 4096, MAX_LARGE = 65536,
    PAGE = 16384 /* mapping granularity on the console */
};
#define MAGIC_SMALL UINT64_C(0x70775f68656170c5)
#define MAGIC_LARGE UINT64_C(0x70775f6865617071)
// In front of an aligned block's pointer, inside the block that holds it:
// class_index is how far back that block's pointer is.
#define MAGIC_ALIGNED UINT64_C(0x70775f68656170a1)

typedef struct Header { uint64_t magic; uint32_t class_index; uint32_t requested; } Header;
typedef struct FreeBlock { struct FreeBlock *next; } FreeBlock;
typedef struct LargeHeader { uint64_t mapped; uint64_t requested; } LargeHeader;
_Static_assert(sizeof(Header) == HEADER, "header size");
_Static_assert(sizeof(LargeHeader) == HEADER, "large header size");

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static FreeBlock *free_lists[CLASSES];
static uint8_t *spans[MAX_SPANS];
static uint32_t span_count;
static uint8_t *carve, *carve_end;
// Live large blocks by caller pointer, sorted: a header is only ever read
// behind a pointer this heap has handed out.
static void *larges[MAX_LARGE];
static uint32_t large_count;
static PlaydenHeapStats stats;

static void *map(size_t bytes)
{
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        stats.last_map_bytes = bytes;
        stats.last_map_errno = (uint64_t)errno;
        return NULL;
    }
    return p;
}

static void note_mapped(int64_t delta)
{
    stats.mapped_bytes += (uint64_t)delta;
    if (stats.mapped_bytes > stats.peak_mapped_bytes) stats.peak_mapped_bytes = stats.mapped_bytes;
}

static void note_live(int64_t delta)
{
    stats.live_bytes += (uint64_t)delta;
    if (stats.live_bytes > stats.peak_live_bytes) stats.peak_live_bytes = stats.live_bytes;
}

static unsigned class_for(size_t bytes)
{
    unsigned index = 0;
    size_t block = (size_t)1 << MIN_SHIFT;
    while (block < bytes + HEADER) { block <<= 1; index++; }
    return index;
}

static size_t class_bytes(unsigned index) { return (size_t)1 << (MIN_SHIFT + index); }

// Spans are kept sorted so ownership is a binary search.
static int owned_span(const void *pointer)
{
    uint32_t low = 0, high = span_count;
    while (low < high) {
        uint32_t mid = (low + high) / 2;
        if ((const uint8_t *)pointer < spans[mid]) high = mid;
        else if ((const uint8_t *)pointer >= spans[mid] + SPAN_BYTES) low = mid + 1;
        else return 1;
    }
    return 0;
}

static int add_span(void)
{
    if (span_count == MAX_SPANS) return 0;
    uint8_t *span = map(SPAN_BYTES);
    if (!span) return 0;
    uint32_t at = span_count;
    while (at && spans[at - 1] > span) { spans[at] = spans[at - 1]; at--; }
    spans[at] = span;
    span_count++;
    note_mapped(SPAN_BYTES);
    carve = span;
    carve_end = span + SPAN_BYTES;
    return 1;
}

static Header *small_alloc(unsigned index)
{
    FreeBlock *block = free_lists[index];
    if (block) { free_lists[index] = block->next; return (Header *)block; }
    size_t bytes = class_bytes(index);
    if ((size_t)(carve_end - carve) < bytes && !add_span()) return NULL;
    Header *header = (Header *)carve;
    carve += bytes;
    return header;
}

static Header *small_header(const void *pointer)
{
    if (!pointer || ((uintptr_t)pointer & (HEADER - 1)) || !owned_span(pointer)) return NULL;
    Header *header = (Header *)((uint8_t *)pointer - HEADER);
    return header->magic == MAGIC_SMALL && header->class_index < CLASSES ? header : NULL;
}

// Position of pointer in the sorted large registry, or where it would go.
static uint32_t large_slot(const void *pointer)
{
    uint32_t low = 0, high = large_count;
    while (low < high) {
        uint32_t mid = (low + high) / 2;
        if ((const uint8_t *)larges[mid] < (const uint8_t *)pointer) low = mid + 1;
        else high = mid;
    }
    return low;
}

// A large block: [LargeHeader][Header][data]; the mapping starts at the first.
static Header *large_header(const void *pointer)
{
    uint32_t slot = large_slot(pointer);
    if (!pointer || slot == large_count || larges[slot] != pointer) return NULL;
    return (Header *)((uint8_t *)pointer - HEADER);
}

static void *allocate(size_t bytes)
{
    if (bytes > UINT32_MAX - PAGE) {
        stats.failures++;
        stats.last_failed_bytes = bytes;
        return NULL;
    }
    Header *header;
    if (bytes + HEADER <= class_bytes(CLASSES - 1)) {
        unsigned index = class_for(bytes);
        if (!(header = small_alloc(index))) {
            stats.failures++;
            stats.last_failed_bytes = bytes;
            return NULL;
        }
        *header = (Header){MAGIC_SMALL, index, (uint32_t)bytes};
    } else {
        size_t mapped = (bytes + HEADER + sizeof(LargeHeader) + PAGE - 1) / PAGE * PAGE;
        LargeHeader *large = large_count < MAX_LARGE ? map(mapped) : NULL;
        if (!large) {
            stats.failures++;
            stats.last_failed_bytes = bytes;
            return NULL;
        }
        *large = (LargeHeader){mapped, bytes};
        note_mapped((int64_t)mapped);
        header = (Header *)(large + 1);
        *header = (Header){MAGIC_LARGE, CLASSES, (uint32_t)bytes};
        uint32_t slot = large_slot(header + 1);
        memmove(&larges[slot + 1], &larges[slot], (large_count - slot) * sizeof(larges[0]));
        larges[slot] = header + 1;
        large_count++;
    }
    stats.allocations++;
    note_live((int64_t)header->requested);
    return header + 1;
}

static void release(Header *header)
{
    note_live(-(int64_t)header->requested);
    stats.frees++;
    if (header->magic == MAGIC_SMALL) {
        unsigned index = header->class_index;
        header->magic = 0;
        FreeBlock *block = (FreeBlock *)header;
        block->next = free_lists[index];
        free_lists[index] = block;
    } else {
        LargeHeader *large = (LargeHeader *)header - 1;
        uint64_t mapped = large->mapped;
        uint32_t slot = large_slot(header + 1);
        memmove(&larges[slot], &larges[slot + 1], (large_count - slot - 1) * sizeof(larges[0]));
        large_count--;
        header->magic = 0;
        (void)munmap(large, mapped);
        note_mapped(-(int64_t)mapped);
    }
}

static Header *owned(const void *pointer)
{
    Header *header = small_header(pointer);
    return header ? header : large_header(pointer);
}

// The block holding an aligned pointer, read only once the pointer is known
// to lie inside memory this heap handed out.
static Header *owned_aligned(const void *pointer)
{
    const uint8_t *p = pointer;
    if (!p || ((uintptr_t)p & (HEADER - 1))) return NULL;
    if (!owned_span(p)) {
        uint32_t slot = large_slot(p);
        if (!slot) return NULL;
        const uint8_t *start = larges[slot - 1];
        if (p <= start || p >= start + ((Header *)(start - HEADER))->requested) return NULL;
    }
    Header *mark = (Header *)(p - HEADER);
    if (mark->magic != MAGIC_ALIGNED || mark->class_index < HEADER ||
        mark->class_index > (uintptr_t)p)
        return NULL;
    return owned(p - mark->class_index);
}

void *__wrap_malloc(size_t bytes)
{
    pthread_mutex_lock(&lock);
    void *result = allocate(bytes);
    pthread_mutex_unlock(&lock);
    return result;
}

void *__wrap_calloc(size_t count, size_t bytes)
{
    if (bytes && count > SIZE_MAX / bytes) {
        pthread_mutex_lock(&lock);
        stats.failures++;
        pthread_mutex_unlock(&lock);
        return NULL;
    }
    void *result = __wrap_malloc(count * bytes);
    // Large blocks are fresh anonymous mappings and already zero.
    if (result && count * bytes + HEADER <= class_bytes(CLASSES - 1)) memset(result, 0, count * bytes);
    return result;
}

static void *aligned(size_t alignment, size_t bytes)
{
    if (!alignment || (alignment & (alignment - 1))) return NULL;
    if (alignment <= HEADER) return __wrap_malloc(bytes);
    if (bytes > SIZE_MAX - alignment || alignment > UINT32_MAX) {
        pthread_mutex_lock(&lock);
        stats.failures++;
        pthread_mutex_unlock(&lock);
        return NULL;
    }
    pthread_mutex_lock(&lock);
    uint8_t *base = allocate(bytes + alignment), *result = NULL;
    if (base) {
        // base is 16-byte aligned, so the gap is 16..alignment bytes and the
        // mark in front of result stays inside the block.
        result = (uint8_t *)(((uintptr_t)base + HEADER + alignment - 1) & ~(uintptr_t)(alignment - 1));
        *(Header *)(result - HEADER) = (Header){MAGIC_ALIGNED, (uint32_t)(result - base), (uint32_t)bytes};
    }
    pthread_mutex_unlock(&lock);
    return result;
}

int __wrap_posix_memalign(void **out, size_t alignment, size_t bytes)
{
    if (!alignment || (alignment & (alignment - 1)) || alignment % sizeof(void *)) return EINVAL;
    void *pointer = aligned(alignment, bytes);
    if (!pointer) return ENOMEM;
    *out = pointer;
    return 0;
}

void __wrap_free(void *pointer)
{
    if (!pointer) return;
    pthread_mutex_lock(&lock);
    Header *header = owned(pointer);
    if (header) {
        release(header);
    } else if ((header = owned_aligned(pointer))) {
        ((Header *)((uint8_t *)pointer - HEADER))->magic = 0;
        release(header);
    } else {
        stats.foreign_frees++;
        pthread_mutex_unlock(&lock);
        __real_free(pointer);
        return;
    }
    pthread_mutex_unlock(&lock);
}

void *__wrap_realloc(void *pointer, size_t bytes)
{
    if (!pointer) return __wrap_malloc(bytes);
    if (!bytes) { __wrap_free(pointer); return NULL; }
    pthread_mutex_lock(&lock);
    Header *header = owned(pointer);
    if (!header && (header = owned_aligned(pointer))) {
        // An aligned block moves to an ordinary one, as realloc allows.
        size_t old = ((Header *)((uint8_t *)pointer - HEADER))->requested;
        void *moved = allocate(bytes);
        if (moved) {
            memcpy(moved, pointer, old < bytes ? old : bytes);
            ((Header *)((uint8_t *)pointer - HEADER))->magic = 0;
            release(header);
        }
        pthread_mutex_unlock(&lock);
        return moved;
    }
    if (!header) {
        // Allocated inside libc; libc knows its size.
        stats.foreign_reallocs++;
        pthread_mutex_unlock(&lock);
        return __real_realloc(pointer, bytes);
    }
    size_t old = header->requested;
    if (header->magic == MAGIC_SMALL && bytes + HEADER <= class_bytes(header->class_index)) {
        note_live((int64_t)bytes - (int64_t)old);
        header->requested = (uint32_t)bytes;
        pthread_mutex_unlock(&lock);
        return pointer;
    }
    void *moved = allocate(bytes);
    if (moved) {
        memcpy(moved, pointer, old < bytes ? old : bytes);
        release(header);
    }
    pthread_mutex_unlock(&lock);
    return moved;
}

void playden_heap_stats(PlaydenHeapStats *out)
{
    if (!out) return;
    pthread_mutex_lock(&lock);
    *out = stats;
    pthread_mutex_unlock(&lock);
}
