#pragma GCC system_header

#ifndef OS_LAYER_H_
#define OS_LAYER_H_

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) || defined(_WIN64)
#define HIVE_OS_WIN32
#include <windows.h>
#else
#define HIVE_OS_LINUX
#include <sys/mman.h>
#include <unistd.h>
#endif

size_t hive_sys_pagesize(void);

void *hive_map(uintptr_t addr, size_t capacity);
bool hive_unmap(void *ptr, size_t capacity);

#endif  // OS_LAYER_H_
