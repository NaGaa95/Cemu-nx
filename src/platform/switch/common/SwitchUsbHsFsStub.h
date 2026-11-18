// Switch 2 stand-in for libusbhsfs: kaiHEN never answers usb:hs (later programs then hang until a reboot), so USB
// storage is reported as absent.
#pragma once

#if !defined(__SWITCH2__)
#error "SwitchUsbHsFsStub.h is only for the Switch 2 build"
#endif

#include <switch.h>

typedef struct {
	u32 usb_if_id;
	u8 lun;
	u32 fs_idx;
	bool write_protect;
	u16 vid;
	u16 pid;
	char manufacturer[64];
	char product_name[64];
	char serial_number[64];
	u64 capacity;
	char name[32];
	u8 fs_type;
	u32 flags;
} UsbHsFsDevice;

#define LIBUSBHSFS_FS_TYPE_STR(x) ((void)(x), "USB")

typedef void (*UsbHsFsPopulateCb)(const UsbHsFsDevice *devices, u32 device_count, void *user_data);

enum {
	UsbHsFsMountFlags_None = 0,
	UsbHsFsMountFlags_ReplayJournal = 1u << 3,
	UsbHsFsMountFlags_ShowHiddenFiles = 1u << 5,
};

static inline Result usbHsFsInitialize(u8 event_idx) {
	(void)event_idx;
	return MAKERESULT(Module_Libnx, LibnxError_NotFound);
}
static inline void usbHsFsExit(void) {}
static inline void usbHsFsSetPopulateCallback(UsbHsFsPopulateCb cb, void *user_data) {
	(void)cb;
	(void)user_data;
}
static inline void usbHsFsSetFileSystemMountFlags(u32 flags) {
	(void)flags;
}
static inline u32 usbHsFsListMountedDevices(UsbHsFsDevice *out, u32 max_count) {
	(void)out;
	(void)max_count;
	return 0;
}
static inline bool usbHsFsUnmountDevice(const UsbHsFsDevice *device, bool signal_status_event) {
	(void)device;
	(void)signal_status_event;
	return false;
}
