// nx2_platform: Nintendo Switch 2 (kaiHEN + libounce) platform helpers shared by the NX ports.
// SPDX-License-Identifier: MIT
//
// Compiled only for the Switch 2 build (__SWITCH2__, set by libounce's toolchain). It covers what differs from the
// Switch 1:
//
// - The process's cores: 6 on the Switch 2 (core mask 0x3F).
// - Input: libounce aborts when kaiHEN refuses the keyboard or the mouse, and the touch screen reports in the hosting
//   game's output range: touch coordinates are set to the Switch's 1280x720.
// - The CPU boost: CancelCpuBoostMode leaves the boost (and FastLoad's low GPU clock) on, so a boost always ends with
//   SetCpuBoostMode(Normal).
// - The display: 1920x1080 at 120 Hz handheld. Docked, the TV's resolution up to 2160p at 60 Hz, or up to 1440p at
//   120 Hz while frame generation is on (the console's 120 Hz TV modes are 1440p and 1080p). 120 Hz goes through
//   nwindowSetFrameRates with VRR in place of the swap interval, which would withdraw libounce's 120 Hz request.
//   drako3d, NVK and Mesa's GL all present at the default window's interval, so this one setting paces every
//   renderer. Mesa sets a swap interval itself when it creates an EGL surface or a Vulkan swapchain and on
//   eglSwapInterval: apply the pacing again after those.
// - JIT code: under kaiHEN, a Jit's RW address is a staging copy, not an alias of its RX mapping. Code becomes
//   executable only once published (kaihenJitPublishRanges, a request to kaiHEN's RO). Publishes made inside a
//   batch are collected per thread and sent as one request when the outermost batch ends.
// - The GPU between users: Mesa (GL and Vulkan) retires its device when its last user ends, and the default window's
//   VI2 layer holds a GSV root that lends the GPU only once. nx2MesaGpuRecover and nx2GpuReleaseForDrako3d hand the
//   GPU over between Mesa, drako3d and a program's next stage.
// - Exit: the homebrew loader runs the next program (hbmenu) in this same process. Unjoined threads would keep their
//   stacks mirrored (and heap pages locked) and running threads would keep running, so an exit cleanup tracks every
//   pthread and releases them after the C++ static destructors. A lookup of usb:hs (never answered under kaiHEN,
//   which would block every later program's lookups) is refused.
//
// Link every program that compiles this file with:
//   -Wl,--wrap=hidInitializeKeyboard -Wl,--wrap=hidInitializeMouse -Wl,--wrap=hidInitializeTouchScreen
//   -Wl,--wrap=__syscall_thread_create -Wl,--wrap=smGetServiceWrapper (the exit cleanup and the usb:hs refusal)
#pragma once

#if defined(__SWITCH2__)

#include <switch.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------------------------------------------------ CPU
u64 nx2CoreMask(void);   // the cores this process may use
int nx2CoreCount(void);
// The n-th core of the mask (wrapping around), for spreading threads; -1 when the mask is empty.
int nx2CoreAt(unsigned n);
// FastLoad (on) or Normal (off): never CancelCpuBoostMode, which leaves the boost on.
Result nx2SetCpuBoost(bool on);

// ------------------------------------------------------------------------------------------------ display
// Sets the handheld and docked sizes and the refresh rate. Call once at start.
void nx2DisplayInit(void);
// Frame generation on or off: docked, it switches the output between up to 2160p at 60 Hz and up to 1440p at
// 120 Hz. True when the refresh rate changed (the pacing then needs nx2DisplayApply or nx2DisplayReapply).
bool nx2DisplaySetFrameGeneration(bool enabled);
// 120 handheld or with frame generation docked, else 60.
int nx2DisplayRefreshRate(void);
// The present pacing for every renderer: at 120 Hz every refresh with VRR (the swap interval is not used), else
// nwindowSetSwapInterval(swap_interval). Cheap when nothing changed.
Result nx2DisplayApply(int swap_interval);
// nx2DisplayApply again with the last swap interval (1 before any): after Mesa set its own.
Result nx2DisplayReapply(void);
// Follows docking and TV resolution changes, and applies a refresh rate change to the pacing: true when the default
// window's size changed since the last call.
bool nx2DisplayUpdate(void);
void nx2DisplaySize(u32* width, u32* height);
bool nx2Docked(void);

// ------------------------------------------------------------------------------------------------ GPU handover
// A program without Mesa links too: the calls then only do their drako3d/VI2 part.
//
// Before Mesa (GL or Vulkan) uses the GPU again after another user ended: an EGL display terminated, drako3d, the
// framebuffer console. An open, usable Mesa device is left as it is. Otherwise what Mesa still holds is retired the
// way it is at exit, and the VI2 layer is released so that the next open gets a new GSV root. Nothing may present
// to the default window meanwhile.
void nx2MesaGpuRecover(void);
// Right before drako3d creates its device: retires Mesa's GPU completely and releases the VI2 layer, so that drako3d
// opens the GPU on a new GSV root. Everything GL- or Vulkan-based must be destroyed first. False when the layer could
// not be released.
bool nx2GpuReleaseForDrako3d(void);

// ------------------------------------------------------------------------------------------------ JIT
void nx2JitRegister(Jit* jit);
void nx2JitUnregister(Jit* jit);
// Publishes [rw_start, rw_start + size) of a registered Jit's staging buffer to its RX view. Inside a batch the
// range is queued; otherwise it is sent now. Returns false when the range is in no registered Jit or RO refused it.
bool nx2JitPublish(const void* rw_start, size_t size);
// The same for an executable (RX) address range: its staging copy is published.
bool nx2JitPublishRx(const void* rx_start, size_t size);
// Either: a range of a registered Jit's staging copy or of its code. Memory in no Jit needs nothing (false).
bool nx2JitPublishAny(const void* start, size_t size);
// Nestable per thread: publishes inside are collected and sent together at the outermost end.
void nx2JitBatchBegin(void);
void nx2JitBatchEnd(void);
// The staging address of an RX address in a registered Jit (NULL if none), for code that reads its own code back.
void* nx2JitRwFromRx(const void* rx);

// ------------------------------------------------------------------------------------------------ exit
// With the HOME exit lock taken (appletLockExit), the system ends the process as soon as it is released: the exit
// cleanup releases it as its last step instead (call this in place of appletUnlockExit at the end of main).
void nx2ExitReleaseLockLast(void);
// For a program that ends with _Exit (skipping atexit handlers and C++ static destructors, which can hang on threads
// nobody stopped): runs the exit cleanup now. Call it right before _Exit; it runs only once.
void nx2ExitCleanupNow(void);

#ifdef __cplusplus
}
#endif

#endif  // __SWITCH2__
