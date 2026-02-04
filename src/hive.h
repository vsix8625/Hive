#ifndef HIVE_H_
#define HIVE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    bool hive_init(void);
    void hive_shutdown(void);

    struct hive_cell *hive_cell_create(const char *name, size_t capacity);
    void hive_cell_destroy(struct hive_cell *cell);

    void *hive_cell_align_alloc(struct hive_cell *cell, size_t size, size_t align);
    void *hive_cell_alloc(struct hive_cell *cell, size_t size);
    void hive_cell_soft_reset(struct hive_cell *cell);
    void hive_cell_reset(struct hive_cell *cell);
    void hive_cell_log_stats(const struct hive_cell *cell);

    // getters
    void *hive_cell_get_base(const struct hive_cell *cell);
    const char *hive_cell_get_name(const struct hive_cell *cell);
    size_t hive_cell_get_used(const struct hive_cell *cell);
    size_t hive_cell_get_capacity(const struct hive_cell *cell);

#if defined(DEBUG)
    size_t hive_cell_get_peak(const struct hive_cell *cell);
    size_t hive_cell_get_alloc_count(const struct hive_cell *cell);
#endif

    static inline size_t hive_nxtpow2(size_t v)
    {
        if (v == 0)
        {
            return 1;
        }
        v--;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        v |= v >> 32;
        v++;
        return v;
    }

    static inline size_t hive_align_up(size_t v, size_t align)
    {
        return (v + align - 1) & ~(align - 1);
    }

#ifdef __cplusplus
}
#endif

#endif  // HIVE_H_
