#ifndef HEAP_REAPER_H_
#define HEAP_REAPER_H_

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct reaper_ctx reaper_ctx;

    typedef struct reaper_stats
    {
        size_t total_bytes;
        size_t peak_bytes;
        size_t active_count;
        size_t total_allocs;
        size_t total_frees;
        size_t largest_alloc;
    } reaper_stats;

    //----------------------------------------------------------------------------------------------------
    // -------------------------
    // Context management
    // -------------------------

    reaper_ctx *reaper_create_ctx(const char *name, bool thread_safe);
    void reaper_destroy_ctx(reaper_ctx *ctx);

    size_t reaper_ctx_total_bytes(const reaper_ctx *ctx);
    size_t reaper_ctx_peak_bytes(const reaper_ctx *ctx);
    const char *reaper_ctx_name(const reaper_ctx *ctx);

    // -------------------------
    // Global ctx
    // -------------------------
    bool reaper_init(void);                    // initializes global ctx
    void reaper_shutdown(void);                // destroys global ctx
    void reaper_destroy_tag(const char *tag);  // destroys allocations in global ctx matching tag

    // -------------------------
    // Allocations (global ctx by default)
    // -------------------------
    void *reaper_malloc(size_t size);
    void *reaper_calloc(size_t n, size_t size);
    void *reaper_realloc(void *ptr, size_t size);
    void reaper_free(void *ptr);
    char *reaper_strdup(const char *s);

    // -------------------------
    // Allocations (specific ctx)
    // -------------------------
    void *reaper_malloc_ctx(reaper_ctx *ctx, size_t size, const char *tag);
    void *reaper_calloc_ctx(reaper_ctx *ctx, size_t n, size_t size, const char *tag);
    void *reaper_realloc_ctx(reaper_ctx *ctx, void *ptr, size_t size);
    void reaper_free_ctx(reaper_ctx *ctx, void *ptr);
    char *reaper_strdup_ctx(reaper_ctx *ctx, const char *s, const char *tag);

    // -------------------------
    // Collection
    // -------------------------
    void reaper_collect_all(reaper_ctx *ctx);
    void reaper_collect_tag_ctx(reaper_ctx *ctx, const char *tag);

    // -------------------------
    // Debug / Stats
    // -------------------------
    void reaper_dump_glob(void);
    void reaper_dump_ctx(const reaper_ctx *ctx);

    void reaper_dump_ctx_file(const reaper_ctx *ctx, const char *filename);
    void reaper_dump_glob_file(void);

    reaper_stats reaper_get_stats(reaper_ctx *ctx);
    void reaper_print_stats(const reaper_ctx *ctx);

    //----------------------------------------------------------------------------------------------------

#ifdef __cplusplus
}
#endif

#endif  // HEAP_REAPER_H_
