#include "hive.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ALLOCS_PER_PHASE 2500000
static void *ptrs[ALLOCS_PER_PHASE];

void run_phase(const char *name, struct hive_cell *c1, struct hive_cell *c2)
{
    printf("\n>>> STARTING PHASE: %s <<<\n", name);

    clock_t start = clock();

    for (size_t i = 0; i < ALLOCS_PER_PHASE; ++i)
    {
        size_t size = 16 + (rand() % 240);
        int choice = rand() % 2;
        void *p = NULL;

        if (choice == 0)
        {
            p = hive_cell_alloc(c1, size);
        }
        else
        {
            p = hive_cell_alloc(c2, size);
        }

        if (p)
        {
            ptrs[i] = p;

            /* The "Touch": Triggers Demand Paging / Huge Page Commits */
            memset(p, (int) i, size);
        }
    }

    clock_t end = clock();
    double cpu_time_used = ((double) (end - start)) / CLOCKS_PER_SEC;

    printf("Phase '%s' took %f seconds\n", name, cpu_time_used);

    hive_cell_log_stats(c1);
    hive_cell_log_stats(c2);
}

#define RENDERER_SIZE (1024LL * 1024 * 1024)
#define PHYSICS_SIZE (2048LL * 1024 * 1024)

int main(void)
{
    srand((unsigned int) time(NULL));

    if (!hive_init())
    {
        fprintf(stderr, "Hive failed to init!\n");
        return 1;
    }

    struct hive_cell *renderer = hive_cell_create("renderer", RENDERER_SIZE);
    struct hive_cell *physics = hive_cell_create("physics", PHYSICS_SIZE);

    if (!renderer || !physics)
    {
        return 1;
    }

    printf("Allocs per phase: %d", ALLOCS_PER_PHASE);
    /* ---------------- PHASE 1: COLD START ---------------- */
    // This tests first-time page faults and Huge Page creation
    run_phase("COLD START", renderer, physics);

    /* ---------------- PHASE 2: SOFT RESET ---------------- */
    // Just move the cursor back. This is "Warm" because the
    // physical RAM is still mapped by the Kernel.
    printf("\n[Action] Performing SOFT RESET (Reuse Memory)...\n");
    hive_cell_soft_reset(renderer);
    hive_cell_soft_reset(physics);

    run_phase("WARM REUSE", renderer, physics);

    /* ---------------- PHASE 3: HARD RESET ---------------- */
    // Use MADV_DONTNEED. This is "Hard" because we give RAM back
    // to the OS but keep the virtual addresses.
    printf("\n[Action] Performing HARD RESET (Decommit Memory)...\n");
    hive_cell_reset(renderer);
    hive_cell_reset(physics);

    run_phase("HARD RESTART", renderer, physics);

    return 0;
}
