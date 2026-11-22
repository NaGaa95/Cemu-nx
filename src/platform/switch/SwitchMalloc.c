// SPDX-FileCopyrightText: 2026 PalindromicBreadLoaf
// SPDX-License-Identifier: GPL-3.0-or-later
// From Kankohi (src/Common/switch/HorizonMalloc.c).

// Replaces newlib's dlmalloc 2.6 (Switch only; libounce brings mimalloc on Switch 2).
#include <malloc.h>
#include <reent.h>
#include <stdlib.h>

#include <switch/kernel/mutex.h>

static inline int InitialLock(Mutex* lock)
{
	mutexInit(lock);
	return 0;
}

static inline int AcquireLock(Mutex* lock)
{
	mutexLock(lock);
	return 0;
}

#define USE_LOCKS 2
#define MLOCK_T Mutex
#define INITIAL_LOCK(lk) InitialLock(lk)
#define DESTROY_LOCK(lk) (0)
#define ACQUIRE_LOCK(lk) AcquireLock(lk)
#define RELEASE_LOCK(lk) mutexUnlock(lk)
#define TRY_LOCK(lk) mutexTryLock(lk)
static MLOCK_T malloc_global_mutex = 0;

#undef M_TRIM_THRESHOLD
#undef M_MMAP_THRESHOLD

#define USE_DL_PREFIX 1
#define STRUCT_MALLINFO_DECLARED 1
#define HAVE_MMAP 0
#define HAVE_MREMAP 0
#define MORECORE_CONTIGUOUS 1
#define MORECORE_CANNOT_TRIM 1
#define DEFAULT_GRANULARITY ((size_t)1 << 20)
#define malloc_getpagesize ((size_t)0x1000)
#define LACKS_TIME_H 1
#define LACKS_SYS_MMAN_H 1
#define REALLOC_ZERO_BYTES_FREES 1

#include "malloc.c"

void* _malloc_r(struct _reent* reent, size_t size)
{
	(void)reent;
	return dlmalloc(size);
}

void _free_r(struct _reent* reent, void* ptr)
{
	(void)reent;
	dlfree(ptr);
}

void* _realloc_r(struct _reent* reent, void* ptr, size_t size)
{
	(void)reent;
	return dlrealloc(ptr, size);
}

void* _calloc_r(struct _reent* reent, size_t count, size_t size)
{
	(void)reent;
	return dlcalloc(count, size);
}

void* _memalign_r(struct _reent* reent, size_t alignment, size_t size)
{
	(void)reent;
	return dlmemalign(alignment, size);
}

void* _valloc_r(struct _reent* reent, size_t size)
{
	(void)reent;
	return dlvalloc(size);
}

void* _pvalloc_r(struct _reent* reent, size_t size)
{
	(void)reent;
	return dlpvalloc(size);
}

size_t _malloc_usable_size_r(struct _reent* reent, void* ptr)
{
	(void)reent;
	return dlmalloc_usable_size(ptr);
}

struct mallinfo _mallinfo_r(struct _reent* reent)
{
	(void)reent;
	return dlmallinfo();
}

int _malloc_trim_r(struct _reent* reent, size_t pad)
{
	(void)reent;
	return dlmalloc_trim(pad);
}

int _mallopt_r(struct _reent* reent, int parameter, int value)
{
	(void)reent;
	return dlmallopt(parameter, value);
}

void _malloc_stats_r(struct _reent* reent)
{
	(void)reent;
	dlmalloc_stats();
}
