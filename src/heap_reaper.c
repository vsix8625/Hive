#include "heap_reaper.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct reaper_alloc_info
{
    void *ptr;
    size_t size;
    const char *tag;
    const char *file;
    int32_t line;
} reaper_alloc_info;

struct reaper_ctx
{
    pthread_mutex_t lock;  // reaper_mutex in future

    reaper_alloc_info *items;
    size_t count;
    size_t capacity;
    const char *name;

    size_t total_bytes;
    size_t peak_bytes;

    size_t total_allocs;
    size_t total_frees;

    bool thread_safe;

    char padding[23];
};

//----------------
// Helpers
//----------------

static void reaper_mutex_lock(reaper_ctx *ctx)
{
    if (ctx->thread_safe)
    {
        pthread_mutex_lock(&ctx->lock);
    }
}

static void reaper_mutex_unlock(reaper_ctx *ctx)
{
    if (ctx->thread_safe)
    {
        pthread_mutex_unlock(&ctx->lock);
    }
}

static bool reaper_grow(reaper_ctx *ctx)
{
    size_t new_capacity = ctx->capacity ? ctx->capacity * 2 : 256;
    reaper_alloc_info *new_items = realloc(ctx->items, new_capacity * sizeof(reaper_alloc_info));

    if (new_items == NULL)
    {
        return false;
    }

    ctx->items = new_items;
    ctx->capacity = new_capacity;

    return true;
}

static void reaper_track_allocation_ctx(reaper_ctx *ctx, void *ptr, size_t size, const char *tag, const char *file,
                                        int32_t line)
{
    if (ptr == NULL)
    {
        return;
    }

    reaper_mutex_lock(ctx);

    if (ctx->count >= ctx->capacity)
    {
        if (!reaper_grow(ctx))
        {
            reaper_mutex_unlock(ctx);
            return;
        }
    }

    ctx->items[ctx->count++] = (reaper_alloc_info) {.ptr = ptr, .size = size, .tag = tag, .file = file, .line = line};

    ctx->total_bytes += size;
    if (ctx->total_bytes > ctx->peak_bytes)
    {
        ctx->peak_bytes = ctx->total_bytes;
    }

    ctx->total_allocs += 1;

    reaper_mutex_unlock(ctx);
}

static void reaper_untrack_ctx(reaper_ctx *ctx, void *ptr)
{
    if (ctx == NULL || ptr == NULL)
    {
        return;
    }

    reaper_mutex_lock(ctx);

    for (size_t i = ctx->count; i-- > 0;)
    {
        if (ctx->items[i].ptr == ptr)
        {
            ctx->total_bytes -= ctx->items[i].size;
            ctx->items[i] = ctx->items[--ctx->count];  // keep list in order
            ctx->total_frees += 1;
            break;
        }
    }

    reaper_mutex_unlock(ctx);
}

//----------------
// Context management
//----------------

static bool reaper_init_ctx(reaper_ctx *ctx, const char *name, bool thread_safe)
{
    if (ctx == NULL)
    {
        return false;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->name = name != NULL ? name : "reaper_creeper";
    ctx->thread_safe = thread_safe;
    if (thread_safe && pthread_mutex_init(&ctx->lock, NULL) != 0)
    {
        return false;
    }
    return true;
}

static void reaper_reset_ctx(reaper_ctx *ctx)
{
    if (ctx == NULL)
    {
        return;
    }

    reaper_collect_all(ctx);
    free(ctx->items);
    ctx->items = NULL;
    ctx->count = 0;
    ctx->capacity = 0;

    if (ctx->thread_safe)
    {
        pthread_mutex_destroy(&ctx->lock);
    }
}

reaper_ctx *reaper_create_ctx(const char *name, bool thread_safe)
{
    reaper_ctx *ctx = malloc(sizeof(reaper_ctx));

    if (ctx == NULL)
    {
        return NULL;
    }

    if (!reaper_init_ctx(ctx, name, thread_safe))
    {
        free(ctx);
        return NULL;
    }

    return ctx;
}

void reaper_destroy_ctx(reaper_ctx *ctx)
{
    if (ctx == NULL)
    {
        return;
    }

    reaper_reset_ctx(ctx);
    free(ctx);
}

size_t reaper_ctx_total_bytes(const reaper_ctx *ctx)
{
    if (ctx == NULL)
    {
        return 0;
    }
    return ctx->total_bytes;
}

size_t reaper_ctx_peak_bytes(const reaper_ctx *ctx)
{
    if (ctx == NULL)
    {
        return 0;
    }
    return ctx->peak_bytes;
}

const char *reaper_ctx_name(const reaper_ctx *ctx)
{
    if (ctx == NULL)
    {
        return NULL;
    }
    return ctx->name;
}

//----------------
// GLOBAL context
//----------------

static reaper_ctx g_reaper_global_ctx;

bool reaper_init(void)
{
    return reaper_init_ctx(&g_reaper_global_ctx, "global_ctx", true);
}

void reaper_shutdown(void)
{
    reaper_reset_ctx(&g_reaper_global_ctx);
}

void reaper_destroy_tag(const char *tag)
{
    if (tag == NULL)
    {
        return;
    }

    reaper_mutex_lock(&g_reaper_global_ctx);

    for (size_t i = 0; i < g_reaper_global_ctx.count;)
    {
        reaper_alloc_info *info = &g_reaper_global_ctx.items[i];
        if (info->tag && strcmp(info->tag, tag) == 0)
        {
            if (info->ptr)
            {
                free(info->ptr);
                g_reaper_global_ctx.total_bytes -= info->size;
                info->ptr = NULL;
            }
            g_reaper_global_ctx.items[i] = g_reaper_global_ctx.items[--g_reaper_global_ctx.count];
            continue;
        }
        i++;
    }

    reaper_mutex_unlock(&g_reaper_global_ctx);
}

// -------------------------
// Allocation helpers
// -------------------------

void *reaper_malloc_ctx(reaper_ctx *ctx, size_t size, const char *tag)
{
    if (ctx == NULL)
    {
        return NULL;
    }

    if (size == 0)
    {
        size = 1;
    }

    void *p = malloc(size);

    if (p == NULL)
    {
        fprintf(stderr, "%s: failed to malloc %zu bytes", __func__, size);
        return NULL;
    }
    reaper_track_allocation_ctx(ctx, p, size, tag, NULL, 0);
    return p;
}

void *reaper_calloc_ctx(reaper_ctx *ctx, size_t n, size_t size, const char *tag)
{
    if (size == 0)
    {
        size = 1;
    }
    if (n == 0)
    {
        n = 1;
    }

    void *p = calloc(n, size);

    if (p == NULL)
    {
        fprintf(stderr, "%s: failed to calloc %zu bytes", __func__, n * size);
        return NULL;
    }

    reaper_track_allocation_ctx(ctx, p, n * size, tag, NULL, 0);
    return p;
}

void *reaper_realloc_ctx(reaper_ctx *ctx, void *ptr, size_t size)
{
    if (size == 0)
    {
        size = 1;
    }

    reaper_mutex_lock(ctx);

    const char *tag = NULL;
    size_t old_size = 0;
    for (size_t i = ctx->count; i-- > 0;)
    {
        if (ctx->items[i].ptr == ptr)
        {
            old_size = ctx->items[i].size;
            tag = ctx->items[i].tag;  // preserve the original tag
            break;
        }
    }
    reaper_mutex_unlock(ctx);

    void *new_ptr = reaper_malloc_ctx(ctx, size, tag);  // track new allocation properly

    if (new_ptr == NULL)
    {
        fprintf(stderr, "%s: failed to malloc %zu bytes\n", __func__, size);
        return NULL;
    }

    if (ptr)
    {
        memcpy(new_ptr, ptr, old_size < size ? old_size : size);
        reaper_free_ctx(ctx, ptr);
    }

    return new_ptr;
}

void reaper_free_ctx(reaper_ctx *ctx, void *ptr)
{
    if (ptr == NULL)
    {
        return;
    }
    reaper_untrack_ctx(ctx, ptr);
    free(ptr);
}

char *reaper_strdup_ctx(reaper_ctx *ctx, const char *s, const char *tag)
{
    if (s == NULL)
    {
        return NULL;
    }
    size_t len = strlen(s) + 1;
    char *copy = reaper_malloc_ctx(ctx, len, tag);
    memcpy(copy, s, len);
    return copy;
}

// -------------------------
// Global alloc macros
// -------------------------
void *reaper_malloc(size_t size)
{
    return reaper_malloc_ctx(&g_reaper_global_ctx, size, "global");
}

void *reaper_calloc(size_t n, size_t size)
{
    return reaper_calloc_ctx(&g_reaper_global_ctx, n, size, "global");
}

void *reaper_realloc(void *ptr, size_t size)
{
    return reaper_realloc_ctx(&g_reaper_global_ctx, ptr, size);
}

void reaper_free(void *ptr)
{
    reaper_free_ctx(&g_reaper_global_ctx, ptr);
}

char *reaper_strdup(const char *s)
{
    return reaper_strdup_ctx(&g_reaper_global_ctx, s, "global");
}

// -------------------------
// Collection
// -------------------------
void reaper_collect_all(reaper_ctx *ctx)
{
    if (ctx == NULL || ctx->items == NULL)
    {
        return;
    }
    reaper_mutex_lock(ctx);

    for (size_t i = ctx->count; i-- > 0;)
    {
        if (ctx->items[i].ptr)
        {
            free(ctx->items[i].ptr);
            ctx->total_frees += 1;
        }
    }
    ctx->count = 0;
    ctx->total_bytes = 0;

    reaper_mutex_unlock(ctx);
}

void reaper_collect_tag_ctx(reaper_ctx *ctx, const char *tag)
{
    if (tag == NULL || ctx->items == NULL)
    {
        return;
    }
    reaper_mutex_lock(ctx);

    for (size_t i = 0; i < ctx->count;)
    {
        reaper_alloc_info *info = &ctx->items[i];
        if (info->tag && strcmp(info->tag, tag) == 0)
        {
            if (info->ptr)
            {
                free(info->ptr);
                ctx->total_frees += 1;
            }
            ctx->total_bytes -= info->size;
            ctx->items[i] = ctx->items[--ctx->count];
            continue;
        }
        i++;
    }

    reaper_mutex_unlock(ctx);
}

// -------------------------
// Debug / Stats
// -------------------------

void reaper_dump_glob(void)
{
    for (size_t i = 0; i < g_reaper_global_ctx.count; ++i)
    {
        const reaper_alloc_info *info = &g_reaper_global_ctx.items[i];
        printf("[%zu] %p (%zu bytes) tag=%s\n", i, info->ptr, info->size, info->tag ? info->tag : "N/A");
    }
}

void reaper_dump_ctx(const reaper_ctx *ctx)
{
    if (ctx == NULL)
    {
        return;
    }

    for (size_t i = 0; i < ctx->count; ++i)
    {
        const reaper_alloc_info *info = &ctx->items[i];
        printf("[%zu] %p (%zu bytes) tag=%s\n", i, info->ptr, info->size, info->tag ? info->tag : "N/A");
    }
}

void reaper_dump_ctx_file(const reaper_ctx *ctx, const char *filename)
{
    if (ctx == NULL || filename == NULL)
    {
        return;
    }

    FILE *fp = fopen(filename, "w");
    if (fp == NULL)
    {
        return;
    }

    for (size_t i = 0; i < ctx->count; ++i)
    {
        const reaper_alloc_info *info = &ctx->items[i];
        fprintf(fp, "[%zu]: %p (%zu bytes) context_name=%s tag=%s\n", i, info->ptr, info->size, ctx->name, info->tag);
    }

    fclose(fp);
}

void reaper_dump_glob_file(void)
{
    reaper_dump_ctx_file(&g_reaper_global_ctx, "reaper_global_dump.txt");
}

// stats

reaper_stats reaper_get_stats(reaper_ctx *ctx)
{
    reaper_stats stats = {0};

    if (ctx == NULL)
    {
        return stats;
    }

    reaper_mutex_lock(ctx);

    stats.total_bytes = ctx->total_bytes;
    stats.peak_bytes = ctx->peak_bytes;

    for (size_t i = 0; i < ctx->count; ++i)
    {
        if (ctx->items[i].ptr)
        {
            stats.active_count += 1;
            if (ctx->items[i].size > stats.largest_alloc)
            {
                stats.largest_alloc = ctx->items[i].size;
            }
        }
    }

    stats.total_allocs = ctx->total_allocs;
    stats.total_frees = ctx->total_frees;

    reaper_mutex_unlock(ctx);
    return stats;
}

void reaper_print_stats(const reaper_ctx *ctx)
{
    if (ctx == NULL)
    {
        ctx = &g_reaper_global_ctx;
    }

    reaper_mutex_lock((reaper_ctx *) ctx);

    size_t active_allocs = ctx->count;
    size_t active_bytes = ctx->total_bytes;

    printf("\n--------------------------------------------------------------------------------\n\n");
    printf("Heap Reaper stats for context: %s\n", ctx->name ? ctx->name : "global");
    printf("  Active allocations: %zu\n", active_allocs);
    printf("  Active bytes:       %zu\n", active_bytes);
    printf("  Peak bytes:         %zu\n", ctx->peak_bytes);
    printf("  Total allocs:       %zu\n", ctx->total_allocs);
    printf("  Total frees:        %zu\n", ctx->total_frees);
    printf("\n\n--------------------------------------------------------------------------------\n");

    reaper_mutex_unlock((reaper_ctx *) ctx);
}
