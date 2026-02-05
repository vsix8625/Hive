#include "os_layer.h"

size_t hive_sys_pagesize(void)
{
#if defined(HIVE_OS_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (size_t) si.dwPageSize;
#else
    return (size_t) sysconf(_SC_PAGESIZE);
#endif
}

void *hive_map(uintptr_t addr, size_t capacity)
{
#if defined(HIVE_OS_WIN32)
    return VirtualAlloc((LPVOID) addr, capacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *ptr = mmap((void *) addr, capacity, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return ptr == MAP_FAILED ? NULL : ptr;
#endif
}

bool hive_unmap(void *ptr, size_t capacity)
{
#if defined(HIVE_OS_WIN32)
    if (ptr)
    {
        // If succeeds, the return value is nonzero.
        // If fails, the return value is 0 (zero). To get extended error information, call GetLastError.
        return VirtualFree(ptr, 0, MEM_RELEASE;
    }
#else
    if (ptr)
    {
        if (munmap(ptr, capacity) == -1)
        {
            return false;
        }
    }
#endif
    return true;
}
