#include "hive.h"
#include "heap_reaper.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct hive_cell
{
    void *base;

    size_t capacity;
    size_t used;

#if defined(DEBUG)
    size_t total_allocs;
    size_t largest_alloc;
    size_t peak;
    size_t page_size;
#endif

    const char *name;
};

enum hive_state : uint32_t
{
    HIVE_STATE_OFF = 0,
    HIVE_STATE_RUNNING,
    HIVE_STATE_SHUTTING_DOWN,
};

static _Atomic enum hive_state g_hive_state = HIVE_STATE_OFF;

struct hive_registry
{
    struct hive_cell **cells;
    size_t count;
    size_t capacity;

    atomic_flag lock;

    char padding[7];
};

//----------------------------------------------------------------------------------------------------

#define HIVE_ADDR_HINT 0x700000000000
#define HIVE_PAGE_SIZE_HUGE 0x00200000

static _Atomic uintptr_t g_hive_cursor = 0;

static size_t g_sys_pagesize = 0;
static struct hive_registry g_hive_registry = {0};

//----------------------------------------------------------------------------------------------------

static void hive_lock(atomic_flag *lock)
{
    while (atomic_flag_test_and_set_explicit(lock, memory_order_acquire))
    {
#if defined(__x86_64__)
        __builtin_ia32_pause();
#endif
    }
}

static void hive_unlock(atomic_flag *lock)
{
    atomic_flag_clear_explicit(lock, memory_order_release);
}

static void register_cell(struct hive_cell *cell)
{
    if (cell == NULL)
    {
        return;
    }

    hive_lock(&g_hive_registry.lock);

    if (g_hive_registry.count >= g_hive_registry.capacity)
    {
        size_t new_cap = g_hive_registry.capacity ? g_hive_registry.capacity * 2 : 256;

        if (new_cap <= g_hive_registry.capacity)
        {
            fprintf(stderr, "%s: registry size overflow\n", __func__);
            hive_unlock(&g_hive_registry.lock);
            return;
        }

        struct hive_cell **tmp = reaper_realloc(g_hive_registry.cells, new_cap * sizeof(struct hive_cell *));

        if (tmp == NULL)
        {
            fprintf(stderr, "%s: failed to realloc hive_registry\n", __func__);
            hive_unlock(&g_hive_registry.lock);
            return;
        }

        g_hive_registry.cells = tmp;
        g_hive_registry.capacity = new_cap;
    }

    g_hive_registry.cells[g_hive_registry.count++] = cell;
    hive_unlock(&g_hive_registry.lock);
}

static void unregister_cell(struct hive_cell *cell)
{
    if (cell == NULL)
    {
        return;
    }

    hive_lock(&g_hive_registry.lock);

    for (size_t i = 0; i < g_hive_registry.count; ++i)
    {
        if (g_hive_registry.cells[i] == cell)
        {
            g_hive_registry.cells[i] = g_hive_registry.cells[g_hive_registry.count - 1];
            g_hive_registry.count--;
            break;
        }
    }

    hive_unlock(&g_hive_registry.lock);
}

//----------------------------------------------------------------------------------------------------
// getters

void *hive_cell_get_base(const struct hive_cell *cell)
{
    return cell ? cell->base : NULL;
}

const char *hive_cell_get_name(const struct hive_cell *cell)
{
    if (cell == NULL)
    {
        return NULL;
    }
    return cell->name ? cell->name : NULL;
}

size_t hive_cell_get_used(const struct hive_cell *cell)
{
    return cell ? cell->used : 0;
}

size_t hive_cell_get_capacity(const struct hive_cell *cell)
{
    return cell ? cell->capacity : 0;
}

#if defined(DEBUG)

size_t hive_cell_get_peak(const struct hive_cell *cell)
{
    return cell ? cell->peak : 0;
}

size_t hive_cell_get_alloc_count(const struct hive_cell *cell)
{
    return cell ? cell->total_allocs : 0;
}
#endif

void hive_log_all_stats(void)
{
    hive_lock(&g_hive_registry.lock);

    for (size_t i = 0; i < g_hive_registry.count; i++)
    {
        struct hive_cell *cell = g_hive_registry.cells[i];

        hive_cell_log_stats(cell);
    }

    hive_unlock(&g_hive_registry.lock);
}

void hive_cell_log_stats(const struct hive_cell *cell)
{
    if (cell == NULL)
    {
        return;
    }

    const char *name = cell->name;
    void *base = cell->base;
    size_t used = cell->used;
    size_t cap = cell->capacity;

    uintptr_t end_addr = (uintptr_t) base + used;

    double used_mb = (double) used / (1024.0 * 1024.0);
    double cap_mb = (double) cap / (1024.0 * 1024.0);

    printf("\n=== Hive Stats [%s: %p - 0x%lx] ===\n", name, base, end_addr);
    printf("  Total bytes    : %-10zu (%.2f MB / %.2f MB)\n", used, used_mb, cap_mb);

#if defined(DEBUG)
    size_t peak = cell->peak;
    size_t count = cell->total_allocs;
    double peak_mb = (double) peak / (1024.0 * 1024.0);
    size_t ps = cell->page_size;

    size_t pages_used = (used + ps - 1) / ps;
    size_t pages_total = cap / ps;

    printf("  Peak bytes     : %-10zu (%.2f MB)\n", peak, peak_mb);
    printf("  Total allocs   : %-10zu\n", count);
    printf("  Largest alloc  : %-10zu bytes\n", cell->largest_alloc);
    printf("  Page size      : %zu\n", ps);
    printf("  Pages          : %zu/%zu\n", pages_used, pages_total);
#endif
    printf("==============================================================\n");
}

//----------------------------------------------------------------------------------------------------

struct hive_cell *hive_cell_create(const char *name, size_t capacity)
{
    if (name == NULL || name[0] == '\0')
    {
        name = "hive_cell";
    }

    if (capacity == 0)
    {
        fprintf(stderr, "%s: hive_cell capacity must be greater than zero(0)\n", __func__);
        return NULL;
    }
    capacity = hive_nxtpow2(capacity);

    size_t alignment = (capacity >= HIVE_PAGE_SIZE_HUGE) ? HIVE_PAGE_SIZE_HUGE : g_sys_pagesize;

    capacity = hive_align_up(capacity, alignment);

    uintptr_t hint = atomic_fetch_add(&g_hive_cursor, capacity);

    struct hive_cell *cell = reaper_calloc(1, sizeof(struct hive_cell));

    if (cell == NULL)
    {
        fprintf(stderr, "%s: failed to allocate memory for hive_cell\n", __func__);
        return NULL;
    }

    void *ptr = mmap((void *) hint, capacity, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (ptr == MAP_FAILED)
    {
        fprintf(stderr, "%s: mmap failure: %s\n", __func__, strerror(errno));
        free(cell);
        return NULL;
    }

    cell->name = name;
    cell->base = ptr;
    cell->capacity = capacity;
    cell->used = 0;

    if (alignment == HIVE_PAGE_SIZE_HUGE)
    {
        madvise(cell->base, cell->capacity, MADV_HUGEPAGE);
    }

#if defined(DEBUG)
    cell->page_size = alignment;
#endif

    register_cell(cell);
    return cell;
}

void hive_cell_destroy(struct hive_cell *cell)
{
    if (cell == NULL)
    {
        return;
    }

    unregister_cell(cell);

    if (cell->base)
    {
        if (munmap(cell->base, cell->capacity) == -1)
        {
            fprintf(stderr, "%s: munmap failed for %s: %s\n", __func__, cell->name, strerror(errno));
        }
        cell->base = NULL;
    }

    // NOTE: we could free reaper allocation here or let reaper_shutdown handle it
    reaper_free(cell);
}

bool hive_init(void)
{
    enum hive_state expected = HIVE_STATE_OFF;
    if (!atomic_compare_exchange_strong(&g_hive_state, &expected, HIVE_STATE_RUNNING))
    {
        return false;
    }

    g_sys_pagesize = (size_t) sysconf(_SC_PAGESIZE);

    uintptr_t cursor_expected = 0;
    atomic_compare_exchange_strong(&g_hive_cursor, &cursor_expected, HIVE_ADDR_HINT);

    reaper_init();

    return true;
}

void hive_shutdown(void)
{
    enum hive_state expected = HIVE_STATE_RUNNING;

    if (!atomic_compare_exchange_strong(&g_hive_state, &expected, HIVE_STATE_SHUTTING_DOWN))
    {
        return;
    }

    reaper_shutdown();
}

//----------------------------------------------------------------------------------------------------

void *hive_cell_align_alloc(struct hive_cell *cell, size_t size, size_t align)
{
    if (cell == NULL || size == 0)
    {
        return NULL;
    }

    if (align < 16)
    {
        align = 16;
    }

    size_t start_offset = (cell->used + align - 1) & ~(align - 1);
    size_t end_offset = start_offset + size;

    if (end_offset > cell->capacity)
    {
        fprintf(stderr, "%s: hive_cell '%s' OOM (%zu/%zu)\n", __func__, cell->name, end_offset, cell->capacity);
        return NULL;
    }

    void *ptr = (char *) cell->base + start_offset;
    cell->used = end_offset;

#if defined(DEBUG)
    cell->total_allocs++;
    if (size > cell->largest_alloc)
    {
        cell->largest_alloc = size;
    }
    if (cell->used > cell->peak)
    {
        cell->peak = cell->used;
    }
#endif

    return ptr;
}

void *hive_cell_alloc(struct hive_cell *cell, size_t size)
{
    return hive_cell_align_alloc(cell, size, 16);
}

void hive_cell_soft_reset(struct hive_cell *cell)
{
    if (cell == NULL)
    {
        return;
    }
    cell->used = 0;
}

void hive_cell_reset(struct hive_cell *cell)
{
    if (cell == NULL || cell->used == 0)
    {
        return;
    }

    madvise(cell->base, cell->used, MADV_DONTNEED);

    cell->used = 0;
#if defined(DEBUG)
    cell->total_allocs = 0;
#endif
}
