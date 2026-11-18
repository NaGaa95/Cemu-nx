// nx2_platform: Nintendo Switch 2 (kaiHEN + libounce) platform helpers. See nx2_platform.h.
// SPDX-License-Identifier: MIT
#if defined(__SWITCH2__)

#include "nx2_platform.h"

#include <errno.h>
#include <kaihen.h>
#include <stdlib.h>
#include <string.h>

static u64 nx2Info(u32 type)
{
	u64 value = 0;
	return R_SUCCEEDED(svcGetInfo(&value, type, CUR_PROCESS_HANDLE, 0)) ? value : 0;
}

// ------------------------------------------------------------------------------------------------ HID
// libounce ends the program when HID refuses to activate the keyboard or the mouse, which depends on the game kaiHEN
// runs the homebrew in. Linking with -Wl,--wrap=hidInitializeKeyboard -Wl,--wrap=hidInitializeMouse (SDL calls
// them) turns a refusal into no keyboard or mouse input. ActivateKeyboard is HID's command 31, ActivateMouse 21.
static Result nx2HidActivate(u32 command)
{
	const u64 aruid = appletGetAppletResourceUserId();
	return serviceDispatchIn(hidGetServiceSession(), command, aruid, .in_send_pid = true);
}

void __wrap_hidInitializeKeyboard(void);
void __wrap_hidInitializeKeyboard(void)
{
	(void)nx2HidActivate(31);
}

void __wrap_hidInitializeMouse(void);
void __wrap_hidInitializeMouse(void)
{
	(void)nx2HidActivate(21);
}

// The touch screen reports in the output range the hosting game set (1920x1080 for a Switch 2 game), while every
// port's touch handling (SDL's included) expects the Switch's 1280x720: ask for that range before touch is activated,
// as HID requires (-Wl,--wrap=hidInitializeTouchScreen).
void __real_hidInitializeTouchScreen(void);
void __wrap_hidInitializeTouchScreen(void);
void __wrap_hidInitializeTouchScreen(void)
{
	(void)hidSetTouchScreenOutputRanges(1280, 720);
	__real_hidInitializeTouchScreen();
}

// ------------------------------------------------------------------------------------------------ CPU
u64 nx2CoreMask(void)
{
	static u64 s_mask = 0;
	if (!s_mask)
	{
		u64 mask = nx2Info(InfoType_CoreMask) & 0xFF;
		s_mask = mask ? mask : 0x3F;
	}
	return s_mask;
}

int nx2CoreCount(void)
{
	return __builtin_popcountll(nx2CoreMask());
}

int nx2CoreAt(unsigned n)
{
	const u64 mask = nx2CoreMask();
	const int count = __builtin_popcountll(mask);
	if (!count)
		return -1;
	n %= (unsigned)count;
	u64 rest = mask;
	for (unsigned i = 0; i < n; ++i)
		rest &= rest - 1;
	return __builtin_ctzll(rest);
}

Result nx2SetCpuBoost(bool on)
{
	return appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
}

// ------------------------------------------------------------------------------------------------ display
static Mutex s_displayMutex;
static bool s_frameGeneration = false;
static int s_refreshHz = 120;
static bool s_tvEventOpen = false, s_tvRead = false;
static Event s_tvEvent;
static s32 s_tvWidth = 3840, s_tvHeight = 2160;
static u32 s_dockedWidth = 0, s_dockedHeight = 0;
static u32 s_lastWidth = 0, s_lastHeight = 0;
static bool s_presentApplied = false;
static int s_presentHz = 0, s_presentInterval = -1;

bool nx2Docked(void)
{
	return appletGetOperationMode() == AppletOperationMode_Console;
}

// The TV's resolution (AM's default display resolution), reread when AM signals a change.
static void nx2ReadTvResolution(void)
{
	if (!s_tvEventOpen && R_SUCCEEDED(appletGetDefaultDisplayResolutionChangeEvent(&s_tvEvent)))
		s_tvEventOpen = true;
	const bool changed = s_tvEventOpen && R_SUCCEEDED(eventWait(&s_tvEvent, 0));
	if (s_tvRead && !changed)
		return;
	if (changed)
		eventClear(&s_tvEvent);
	s32 width = 0, height = 0;
	if (R_SUCCEEDED(appletGetDefaultDisplayResolution(&width, &height)) && width > 0 && height > 0)
	{
		s_tvWidth = width;
		s_tvHeight = height;
	}
	s_tvRead = true;
}

// The refresh rate for the operation mode, and the docked size: the TV's resolution, at most 2160 lines, 1440 at
// 120 Hz. Call with s_displayMutex held.
static void nx2ApplyMode(void)
{
	s_refreshHz = !nx2Docked() || s_frameGeneration ? 120 : 60;
	nx2ReadTvResolution();
	u32 width = (u32)s_tvWidth, height = (u32)s_tvHeight;
	const u32 maxHeight = s_frameGeneration ? 1440 : 2160;
	if (height > maxHeight)
	{
		width = (width * maxHeight / height + 1) & ~1u;
		height = maxHeight;
	}
	if (width == s_dockedWidth && height == s_dockedHeight)
		return;
	if (R_SUCCEEDED(nwindowSetModeDimensions(nwindowGetDefault(), 1920, 1080, width, height)))
	{
		s_dockedWidth = width;
		s_dockedHeight = height;
	}
}

// Call with s_displayMutex held.
static Result nx2ApplyPacing(int swap_interval)
{
	if (swap_interval < 0)
		swap_interval = 0;
	Result rc = 0;
	NWindow* window = nwindowGetDefault();
	if (s_refreshHz > 60)
	{
		// (every call: a swap interval set elsewhere, by EGL or drako3d, withdraws the 120 Hz request)
		rc = nwindowSetFrameRates(window, 0, 0, true);
	}
	else if (!s_presentApplied || s_presentHz != s_refreshHz || s_presentInterval != swap_interval)
	{
		rc = nwindowSetSwapInterval(window, (u32)swap_interval);
	}
	s_presentApplied = R_SUCCEEDED(rc);
	s_presentHz = s_refreshHz;
	s_presentInterval = swap_interval;
	return rc;
}

void nx2DisplayInit(void)
{
	mutexLock(&s_displayMutex);
	nx2ApplyMode();
	nwindowGetDimensions(nwindowGetDefault(), &s_lastWidth, &s_lastHeight);
	mutexUnlock(&s_displayMutex);
}

bool nx2DisplaySetFrameGeneration(bool enabled)
{
	if (enabled == s_frameGeneration)
		return false;
	mutexLock(&s_displayMutex);
	const int previousHz = s_refreshHz;
	s_frameGeneration = enabled;
	nx2ApplyMode();
	const bool changed = s_refreshHz != previousHz;
	if (changed)
		s_presentApplied = false;
	mutexUnlock(&s_displayMutex);
	return changed;
}

int nx2DisplayRefreshRate(void)
{
	return s_refreshHz;
}

Result nx2DisplayApply(int swap_interval)
{
	mutexLock(&s_displayMutex);
	const Result rc = nx2ApplyPacing(swap_interval);
	mutexUnlock(&s_displayMutex);
	return rc;
}

Result nx2DisplayReapply(void)
{
	mutexLock(&s_displayMutex);
	s_presentApplied = false;
	const Result rc = nx2ApplyPacing(s_presentInterval < 0 ? 1 : s_presentInterval);
	mutexUnlock(&s_displayMutex);
	return rc;
}

bool nx2DisplayUpdate(void)
{
	mutexLock(&s_displayMutex);
	const int previousHz = s_refreshHz;
	nx2ApplyMode();
	if (s_refreshHz != previousHz)
	{
		s_presentApplied = false;
		nx2ApplyPacing(s_presentInterval < 0 ? 1 : s_presentInterval);
	}
	u32 width = 0, height = 0;
	nwindowGetDimensions(nwindowGetDefault(), &width, &height);
	const bool changed = width != s_lastWidth || height != s_lastHeight;
	s_lastWidth = width;
	s_lastHeight = height;
	mutexUnlock(&s_displayMutex);
	return changed;
}

void nx2DisplaySize(u32* width, u32* height)
{
	u32 w = 1920, h = 1080;
	nwindowGetDimensions(nwindowGetDefault(), &w, &h);
	if (width)
		*width = w;
	if (height)
		*height = h;
}

// ------------------------------------------------------------------------------------------------ GPU handover
// Mesa's exit hook (weak, so that programs without Mesa link: it is NULL there): it drains and closes Mesa's device
// whatever it still holds, then the runtime's GSV sessions and the VI2 layer. After it, any API opens the GPU again.
void __nx_gpu_exit(void) __attribute__((weak));

// Mesa's budget snapshot: false when no device is open and usable (never opened, or retired). The storage is larger
// than the SDK's struct, which it writes whole.
bool nvkmd_nvgpu_native_budget_snapshot(void* out) __attribute__((weak));

static bool nx2MesaDeviceOpen(void)
{
	if (!nvkmd_nvgpu_native_budget_snapshot)
		return false;
	uint64_t snapshot[128];
	memset(snapshot, 0, sizeof(snapshot));
	return nvkmd_nvgpu_native_budget_snapshot(snapshot);
}

void nx2MesaGpuRecover(void)
{
	if (nx2MesaDeviceOpen())
		return;
	if (__nx_gpu_exit)
		__nx_gpu_exit();
	(void)nwindowReleaseVi2Layer(nwindowGetDefault());
}

bool nx2GpuReleaseForDrako3d(void)
{
	if (__nx_gpu_exit)
		__nx_gpu_exit();
	return R_SUCCEEDED(nwindowReleaseVi2Layer(nwindowGetDefault()));
}

// ------------------------------------------------------------------------------------------------ JIT
#define NX2_MAX_JITS 32
#define NX2_MAX_PENDING 128

static Mutex s_jitMutex;
static Jit* s_jits[NX2_MAX_JITS];

typedef struct
{
	Jit* jit;
	unsigned depth;
	size_t count;
	KaihenJitRange ranges[NX2_MAX_PENDING];
} Nx2PendingPublish;

static _Thread_local Nx2PendingPublish s_pending;

void nx2JitRegister(Jit* jit)
{
	mutexLock(&s_jitMutex);
	for (int i = 0; i < NX2_MAX_JITS; ++i)
		if (!s_jits[i])
		{
			s_jits[i] = jit;
			break;
		}
	mutexUnlock(&s_jitMutex);
}

void nx2JitUnregister(Jit* jit)
{
	if (s_pending.jit == jit)
		s_pending.count = 0, s_pending.jit = NULL;
	mutexLock(&s_jitMutex);
	for (int i = 0; i < NX2_MAX_JITS; ++i)
		if (s_jits[i] == jit)
			s_jits[i] = NULL;
	mutexUnlock(&s_jitMutex);
}

// The registered Jit holding p in its staging (rx false) or executable (rx true) view. Call with s_jitMutex held.
static Jit* nx2FindJit(const void* p, bool rx)
{
	const uintptr_t address = (uintptr_t)p;
	for (int i = 0; i < NX2_MAX_JITS; ++i)
	{
		Jit* jit = s_jits[i];
		if (!jit)
			continue;
		const uintptr_t base = (uintptr_t)(rx ? jit->rx_addr : jit->rw_addr);
		if (address >= base && address - base < jit->size)
			return jit;
	}
	return NULL;
}

static int nx2CompareRanges(const void* a, const void* b)
{
	const KaihenJitRange* x = (const KaihenJitRange*)a;
	const KaihenJitRange* y = (const KaihenJitRange*)b;
	return x->offset < y->offset ? -1 : x->offset > y->offset ? 1 : 0;
}

// Sends ranges of one Jit to RO: sorted and merged, as kaihenJitPublishRanges only joins ranges that follow each
// other. Without kaiHEN's staging copy (a real RW/RX alias), only the cache maintenance is needed.
static bool nx2SendRanges(Jit* jit, KaihenJitRange* ranges, size_t count)
{
	if (!count)
		return true;
	qsort(ranges, count, sizeof(*ranges), nx2CompareRanges);
	size_t merged = 0;
	for (size_t i = 1; i < count; ++i)
	{
		KaihenJitRange* last = &ranges[merged];
		if (ranges[i].offset <= last->offset + last->size)
		{
			const u64 end = ranges[i].offset + ranges[i].size;
			if (end > last->offset + last->size)
				last->size = end - last->offset;
		}
		else
			ranges[++merged] = ranges[i];
	}
	count = merged + 1;

	if (jit->type == JitType_SetProcessMemoryPermission && jit->rw_addr != jit->src_addr)
		return R_SUCCEEDED(kaihenJitPublishRanges(jit, ranges, count));
	for (size_t i = 0; i < count; ++i)
	{
		armDCacheFlush((u8*)jit->rw_addr + ranges[i].offset, ranges[i].size);
		armICacheInvalidate((u8*)jit->rx_addr + ranges[i].offset, ranges[i].size);
	}
	return true;
}

static bool nx2FlushPending(void)
{
	Nx2PendingPublish* pending = &s_pending;
	const bool ok = !pending->jit || nx2SendRanges(pending->jit, pending->ranges, pending->count);
	pending->count = 0;
	pending->jit = NULL;
	return ok;
}

static bool nx2Queue(Jit* jit, u64 offset, u64 size)
{
	Nx2PendingPublish* pending = &s_pending;
	if (!pending->depth)
	{
		KaihenJitRange range = {offset, size};
		return nx2SendRanges(jit, &range, 1);
	}
	bool ok = true;
	if ((pending->jit && pending->jit != jit) || pending->count == NX2_MAX_PENDING)
		ok = nx2FlushPending();
	pending->jit = jit;
	pending->ranges[pending->count].offset = offset;
	pending->ranges[pending->count].size = size;
	pending->count++;
	return ok;
}

// Queues [start, start + size) of jit's staging (rx false) or executable (rx true) view, clipped to the Jit.
static bool nx2QueueRange(Jit* jit, bool rx, const void* start, size_t size)
{
	const uintptr_t base = (uintptr_t)(rx ? jit->rx_addr : jit->rw_addr);
	const u64 offset = (u64)((uintptr_t)start - base);
	if (offset + size > jit->size)
		size = jit->size - offset;
	return nx2Queue(jit, offset, size);
}

bool nx2JitPublish(const void* rw_start, size_t size)
{
	if (!size)
		return true;
	mutexLock(&s_jitMutex);
	Jit* jit = nx2FindJit(rw_start, false);
	mutexUnlock(&s_jitMutex);
	return jit && nx2QueueRange(jit, false, rw_start, size);
}

bool nx2JitPublishRx(const void* rx_start, size_t size)
{
	if (!size)
		return true;
	mutexLock(&s_jitMutex);
	Jit* jit = nx2FindJit(rx_start, true);
	mutexUnlock(&s_jitMutex);
	return jit && nx2QueueRange(jit, true, rx_start, size);
}

bool nx2JitPublishAny(const void* start, size_t size)
{
	if (!size)
		return true;
	mutexLock(&s_jitMutex);
	bool rx = false;
	Jit* jit = nx2FindJit(start, false);
	if (!jit)
	{
		jit = nx2FindJit(start, true);
		rx = true;
	}
	mutexUnlock(&s_jitMutex);
	return jit && nx2QueueRange(jit, rx, start, size);
}

void nx2JitBatchBegin(void)
{
	s_pending.depth++;
}

void nx2JitBatchEnd(void)
{
	if (s_pending.depth && --s_pending.depth == 0)
		nx2FlushPending();
}

void* nx2JitRwFromRx(const void* rx)
{
	mutexLock(&s_jitMutex);
	Jit* jit = nx2FindJit(rx, true);
	mutexUnlock(&s_jitMutex);
	return jit ? (u8*)jit->rw_addr + ((uintptr_t)rx - (uintptr_t)jit->rx_addr) : NULL;
}

// ------------------------------------------------------------------------------------------------ exit cleanup
// At exit the homebrew loader runs the next program in this same process: a thread still running would run this
// program's code under the next, and its stack mirror (mapped until a join) would keep the next program short of
// memory. Every pthread is tracked (-Wl,--wrap=__syscall_thread_create); after the C++ static destructors, running
// threads get a second to end, then are paused and their stacks unmapped. libounce releases the threads that ended.
struct __pthread_t;
int __real___syscall_thread_create(struct __pthread_t** thread, void* (*func)(void*), void* arg, void* stack_addr,
								   size_t stack_size);
int __wrap___syscall_thread_create(struct __pthread_t** thread, void* (*func)(void*), void* arg, void* stack_addr,
								   size_t stack_size);

typedef struct
{
	void* (*func)(void*);
	void* arg;
} Nx2ThreadStart;

typedef struct
{
	Thread* thread;
	void* mirror;
	void* memory;
	bool live;
} Nx2TrackedThread;

#define NX2_MAX_TRACKED_THREADS 512
static Nx2TrackedThread s_threads[NX2_MAX_TRACKED_THREADS];
static Mutex s_threadsMutex;
static bool s_releaseExitLock = false;
static bool s_exitCleanupDone = false;

// Whether mirror is still a stack mirror (svcMapMemory'd memory starting there), and its size.
static bool nx2IsStackMirror(void* mirror, u64* size)
{
	MemoryInfo info = {0};
	u32 pageInfo = 0;
	if (R_FAILED(svcQueryMemory(&info, &pageInfo, (u64)(uintptr_t)mirror)))
		return false;
	if (size)
		*size = info.size;
	return (info.type & MemState_Type) == MemType_MappedMemory && info.addr == (u64)(uintptr_t)mirror;
}

static int nx2TrackThreadStart(Thread* self)
{
	mutexLock(&s_threadsMutex);
	int freeSlot = -1;
	for (int i = 0; i < NX2_MAX_TRACKED_THREADS; i++)
	{
		Nx2TrackedThread* entry = &s_threads[i];
		// A finished thread whose stack mirror address came back has been joined: forget it.
		if (!entry->live && entry->mirror == self->stack_mirror)
			memset(entry, 0, sizeof(*entry));
		if (freeSlot < 0 && !entry->live && !entry->mirror)
			freeSlot = i;
	}
	if (freeSlot < 0)
	{
		// Full: forget the finished threads that were joined since.
		for (int i = 0; i < NX2_MAX_TRACKED_THREADS; i++)
		{
			Nx2TrackedThread* entry = &s_threads[i];
			if (!entry->live && entry->mirror && !nx2IsStackMirror(entry->mirror, NULL))
			{
				memset(entry, 0, sizeof(*entry));
				if (freeSlot < 0)
					freeSlot = i;
			}
		}
	}
	if (freeSlot >= 0)
	{
		Nx2TrackedThread* entry = &s_threads[freeSlot];
		entry->thread = self;
		entry->mirror = self->stack_mirror;
		entry->memory = self->stack_mem;
		entry->live = true;
	}
	mutexUnlock(&s_threadsMutex);
	return freeSlot;
}

static void nx2TrackThreadFinish(int slot)
{
	if (slot < 0)
		return;
	mutexLock(&s_threadsMutex);
	s_threads[slot].live = false;
	s_threads[slot].thread = NULL;
	mutexUnlock(&s_threadsMutex);
}

static void* nx2ThreadTrampoline(void* argument)
{
	const Nx2ThreadStart start = *(Nx2ThreadStart*)argument;
	free(argument);
	Thread* self = threadGetSelf();
	const int slot = self ? nx2TrackThreadStart(self) : -1;
	void* result = start.func(start.arg);
	nx2TrackThreadFinish(slot);
	return result;
}

int __wrap___syscall_thread_create(struct __pthread_t** thread, void* (*func)(void*), void* arg, void* stack_addr,
								   size_t stack_size)
{
	Nx2ThreadStart* start = (Nx2ThreadStart*)malloc(sizeof(Nx2ThreadStart));
	if (!start)
		return ENOMEM;
	start->func = func;
	start->arg = arg;
	const int rc = __real___syscall_thread_create(thread, nx2ThreadTrampoline, start, stack_addr, stack_size);
	if (rc != 0)
		free(start);
	return rc;
}

static unsigned nx2CountLiveThreads(void)
{
	unsigned live = 0;
	mutexLock(&s_threadsMutex);
	for (int i = 0; i < NX2_MAX_TRACKED_THREADS; i++)
		live += s_threads[i].live;
	mutexUnlock(&s_threadsMutex);
	return live;
}

void nx2ExitReleaseLockLast(void)
{
	s_releaseExitLock = true;
}

static void nx2ExitCleanup(void)
{
	// (once: nx2ExitCleanupNow may already have run it)
	if (s_exitCleanupDone)
		return;
	s_exitCleanupDone = true;
	for (int waited = 0; waited < 1000 && nx2CountLiveThreads() != 0; waited += 10)
		svcSleepThread(10000000ull);

	const Handle current = threadGetCurHandle();
	mutexLock(&s_threadsMutex);
	for (int i = 0; i < NX2_MAX_TRACKED_THREADS; i++)
	{
		Nx2TrackedThread* entry = &s_threads[i];
		if (entry->live && entry->thread && entry->thread->handle != current)
		{
			// Never resumed: it must not run this program's code under the next one.
			svcSetThreadActivity(entry->thread->handle, ThreadActivity_Paused);
			u64 size = 0;
			// (the mirror covers the stack and libnx's TLS/reent page after it)
			if (entry->mirror && nx2IsStackMirror(entry->mirror, &size))
				svcUnmapMemory(entry->mirror, entry->memory, size);
			memset(entry, 0, sizeof(*entry));
		}
		else if (!entry->live)
		{
			// Ended: libounce's _threadReleaseExited unmaps its stack and closes it after this runs (unmapping it
			// here made that fail and leaked the thread's handle).
			memset(entry, 0, sizeof(*entry));
		}
	}
	mutexUnlock(&s_threadsMutex);

	if (s_releaseExitLock)
	{
		s_releaseExitLock = false;
		appletUnlockExit();
	}
}

void nx2ExitCleanupNow(void)
{
	nx2ExitCleanup();
}

// Registered before any C++ static object is constructed, so it runs after their destructors: the last of this
// program's code before libnx's own teardown.
__attribute__((constructor(102))) static void nx2RegisterExitCleanup(void)
{
	atexit(nx2ExitCleanup);
}

// ------------------------------------------------------------------------------------------------ service lookups
// kaiHEN's SM proxy forwards every program's lookups on one shared SM session, and SM answers a lookup of a service
// nobody registered only once it is registered: such a lookup blocks that session, and every later program's
// lookups (hbmenu's included), until kaiHEN restarts. usb:hs is never answered, so it is refused here
// (-Wl,--wrap=smGetServiceWrapper).
Result __real_smGetServiceWrapper(Service* service_out, SmServiceName name);
Result __wrap_smGetServiceWrapper(Service* service_out, SmServiceName name);

Result __wrap_smGetServiceWrapper(Service* service_out, SmServiceName name)
{
	if (strncmp(name.name, "usb:hs", 6) == 0)
		return MAKERESULT(Module_Libnx, LibnxError_NotFound);
	return __real_smGetServiceWrapper(service_out, name);
}

#endif  // __SWITCH2__
