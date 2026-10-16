#include "SwitchStorage.h"

#include <switch.h>
#include <usbhsfs.h>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#include <sys/iosupport.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>

namespace SwitchStorage
{
namespace
{
constexpr size_t kSmbReadAheadMin = 64 * 1024;
constexpr size_t kSmbReadAheadMax = 512 * 1024;
constexpr size_t kSmbReadAheadBudget = 8 * 1024 * 1024;

struct SmbMount;
struct SmbDevice;

struct SmbFile
{
	std::shared_ptr<SmbMount>* lifetime;
	SmbMount* mount;
	smb2fh* handle;
	uint8_t* readAhead;
	size_t readAheadCapacity;
	size_t readAheadOffset;
	size_t readAheadSize;
	unsigned sequentialReads;
	uint64_t position;
	int flags;
	bool openedOnce;
	char path[PATH_MAX];
};

struct SmbDir
{
	std::shared_ptr<SmbMount>* lifetime;
	SmbMount* mount;
	smb2dir* handle;
	struct CachedEntry
	{
		char name[NAME_MAX]{};
		struct stat info{};
	};
	std::vector<CachedEntry>* entries;
	size_t index;
};

struct SmbMount
{
	SmbShare config;
	SmbDevice* device = nullptr;
	smb2_context* context = nullptr;
	bool connected = false;
	std::atomic<SmbConnectionState> state{SmbConnectionState::Disconnected};
	std::atomic_bool retired{false};
	std::mutex ioMutex;
	size_t readAheadBytes = 0;
	std::unordered_map<std::string, struct stat> directoryMetadata;

	~SmbMount()
	{
		if (context)
		{
			if (connected)
				smb2_disconnect_share(context);
			smb2_destroy_context(context);
		}
	}
};

// Newlib retains devoptab pointers in open descriptors. Keep registrations at
// stable addresses for the process lifetime; each remount gets a fresh
// tombstone so an old callback can never acquire a replacement connection.
struct SmbDevice
{
	std::string deviceName;
	std::string rootPath;
	devoptab_t devoptab{};
	std::shared_ptr<SmbMount> mount;
};

bool EnsureReadAheadBuffer(SmbFile* file, size_t desired)
{
	desired = std::clamp(desired, kSmbReadAheadMin, kSmbReadAheadMax);
	if (file->readAhead && file->readAheadCapacity >= desired)
		return true;
	const size_t additional = desired - file->readAheadCapacity;
	if (file->mount->readAheadBytes > kSmbReadAheadBudget - additional)
		return false;
	void* resized = std::realloc(file->readAhead, desired);
	if (!resized)
		return false;
	file->readAhead = static_cast<uint8_t*>(resized);
	file->readAheadCapacity = desired;
	file->mount->readAheadBytes += additional;
	return true;
}

void ReleaseReadAheadBuffer(SmbFile* file)
{
	if (!file->readAhead)
		return;
	std::free(file->readAhead);
	file->readAhead = nullptr;
	file->mount->readAheadBytes -= file->readAheadCapacity;
	file->readAheadCapacity = 0;
	file->readAheadOffset = 0;
	file->readAheadSize = 0;
	file->sequentialReads = 0;
}

int SynchronizeFilePosition(SmbFile* file)
{
	if (file->readAheadSize == 0)
		return 0;
	uint64_t resultPosition = 0;
	const int result = smb2_lseek(file->mount->context, file->handle,
		static_cast<int64_t>(file->position), SEEK_SET, &resultPosition);
	if (result >= 0)
	{
		file->readAheadOffset = 0;
		file->readAheadSize = 0;
	}
	return result;
}

std::mutex s_mountMutex;
std::vector<std::shared_ptr<SmbMount>> s_smbMounts;
std::vector<std::unique_ptr<SmbDevice>> s_smbDevices;
std::atomic_bool s_usbInitialized{false};
std::mutex s_usbInitMutex;
std::atomic<uint64_t> s_usbGeneration{0};
std::mutex s_usbMutex;
std::vector<UsbHsFsDevice> s_usbDevices;
std::mutex s_usbCallbackMutex;
UsbStatusCallback s_usbCallback = nullptr;
void* s_usbCallbackData = nullptr;

void usbStatusChanged(const UsbHsFsDevice* devices, u32 count, void*)
{
	{
		std::lock_guard<std::mutex> lock(s_usbMutex);
		s_usbDevices.clear();
		if (devices && count)
			s_usbDevices.assign(devices, devices + count);
		s_usbGeneration.fetch_add(1, std::memory_order_release);
	}
	std::lock_guard<std::mutex> callbackLock(s_usbCallbackMutex);
	if (s_usbCallback)
		s_usbCallback(s_usbCallbackData);
}

void hashBytes(uint64_t& hash, const void* data, size_t size)
{
	const auto* bytes = static_cast<const uint8_t*>(data);
	for (size_t index = 0; index < size; ++index) { hash ^= bytes[index]; hash *= 1099511628211ULL; }
}
template <typename T> void hashInteger(uint64_t& hash, T value)
{
	for (size_t index = 0; index < sizeof(T); ++index) { const uint8_t byte = value & 0xff; hashBytes(hash, &byte, 1); value >>= 8; }
}
template <size_t Size> void hashText(uint64_t& hash, const char (&text)[Size])
{
	hashBytes(hash, text, strnlen(text, Size)); const uint8_t separator = 0; hashBytes(hash, &separator, 1);
}
std::string formatUsbId(const char* prefix, uint64_t hash)
{
	char text[32]; std::snprintf(text, sizeof(text), "%s-%016llx", prefix, static_cast<unsigned long long>(hash)); return text;
}
std::string usbPhysicalId(const UsbHsFsDevice& device)
{
	uint64_t hash = 14695981039346656037ULL; hashInteger(hash, device.vid); hashInteger(hash, device.pid); hashText(hash, device.serial_number);
	if (!device.serial_number[0]) { hashText(hash, device.manufacturer); hashText(hash, device.product_name); hashInteger(hash, device.capacity); }
	return formatUsbId("usbdev", hash);
}
std::string usbVolumeId(const UsbHsFsDevice& device)
{
	uint64_t hash = 14695981039346656037ULL; const std::string physical = usbPhysicalId(device); hashBytes(hash, physical.data(), physical.size());
	hashInteger(hash, device.lun); hashInteger(hash, device.fs_idx); hashInteger(hash, device.fs_type); hashInteger(hash, device.capacity); return formatUsbId("usbvol", hash);
}
Location makeUsbLocation(const UsbHsFsDevice& device)
{
	Location location; location.id=usbVolumeId(device); location.physicalId=usbPhysicalId(device); location.mountAlias=device.name; location.path=device.name;
	if (!location.path.empty() && location.path.back()!='/') location.path+='/';
	location.serialNumber.assign(device.serial_number,strnlen(device.serial_number,sizeof(device.serial_number)));
	location.vendorId=device.vid; location.productId=device.pid; location.lun=device.lun; location.partition=device.fs_idx;
	location.filesystemType=device.fs_type; location.capacity=device.capacity;
	const uint64_t gib=device.capacity/(1024ULL*1024ULL*1024ULL); char label[256];
	std::snprintf(label,sizeof(label),"%s - %s%s%s (%llu GiB)",device.name,LIBUSBHSFS_FS_TYPE_STR(device.fs_type),device.product_name[0]?" - ":"",device.product_name,static_cast<unsigned long long>(gib));
	location.label=label; return location;
}

int fail(_reent* reent, int error)
{
	reent->_errno = error > 0 ? error : EIO;
	return -1;
}

std::string trim(std::string value)
{
	const auto first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
		return {};
	const auto last = value.find_last_not_of(" \t\r\n");
	return value.substr(first, last - first + 1);
}

bool validId(const std::string& id)
{
	if (id.empty() || id.size() > 16)
		return false;
	return std::all_of(id.begin(), id.end(), [](unsigned char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		       (c >= '0' && c <= '9') || c == '_';
	});
}

std::string deviceNameForId(const std::string& id)
{
	return validId(id) ? "cemusmb_" + id : std::string{};
}

bool fixPath(const char* source, char* destination, size_t destinationSize)
{
	if (!source || !destination || destinationSize == 0)
		return false;
	const char* colon = std::strchr(source, ':');
	if (!colon)
		return false;
	const char* input = colon + 1;
	while (*input == '/')
		++input;

	size_t length = 0;
	bool slash = false;
	for (; *input; ++input)
	{
		if (*input == '/')
		{
			if (slash)
				continue;
			slash = true;
		}
		else
		{
			slash = false;
		}
		if (length + 1 >= destinationSize)
			return false;
		destination[length++] = *input;
	}
	while (length && destination[length - 1] == '/')
		--length;
	destination[length] = '\0';
	return true;
}

bool isRootPath(const char* path)
{
	const char* colon = path ? std::strchr(path, ':') : nullptr;
	if (!colon)
		return false;
	++colon;
	while (*colon == '/')
		++colon;
	return *colon == '\0';
}

void fillStat(struct stat* output, const struct smb2_stat_64& input)
{
	std::memset(output, 0, sizeof(*output));
	switch (input.smb2_type)
	{
	case SMB2_TYPE_FILE:
		output->st_mode = S_IFREG | 0666;
		break;
	case SMB2_TYPE_DIRECTORY:
		output->st_mode = S_IFDIR | 0777;
		break;
	case SMB2_TYPE_LINK:
		output->st_mode = S_IFLNK | 0777;
		break;
	default:
		output->st_mode = S_IFREG | 0444;
		break;
	}
	output->st_ino = input.smb2_ino;
	output->st_nlink = input.smb2_nlink ? input.smb2_nlink : 1;
	output->st_size = static_cast<off_t>(input.smb2_size);
	output->st_atime = input.smb2_atime;
	output->st_mtime = input.smb2_mtime;
	output->st_ctime = input.smb2_ctime;
	output->st_blksize = 65536;
}

std::shared_ptr<SmbMount> mountFrom(_reent* reent)
{
	auto* device = reent ? static_cast<SmbDevice*>(reent->deviceData) : nullptr;
	if (!device)
		return {};
	std::lock_guard<std::mutex> lock(s_mountMutex);
	auto mount = device->mount;
	if (!mount || mount->retired.load(std::memory_order_acquire))
		return {};
	return mount;
}

std::shared_ptr<SmbMount> mountFrom(const SmbFile* file)
{
	return file && file->lifetime ? *file->lifetime : std::shared_ptr<SmbMount>{};
}

std::shared_ptr<SmbMount> mountFrom(const SmbDir* directory)
{
	return directory && directory->lifetime ? *directory->lifetime : std::shared_ptr<SmbMount>{};
}

bool pinMount(SmbFile* file, std::shared_ptr<SmbMount> mount)
{
	file->lifetime = new (std::nothrow) std::shared_ptr<SmbMount>(std::move(mount));
	if (!file->lifetime)
		return false;
	file->mount = file->lifetime->get();
	return true;
}

bool pinMount(SmbDir* directory, std::shared_ptr<SmbMount> mount)
{
	directory->lifetime = new (std::nothrow) std::shared_ptr<SmbMount>(std::move(mount));
	if (!directory->lifetime)
		return false;
	directory->mount = directory->lifetime->get();
	return true;
}

void releaseMount(SmbFile* file)
{
	auto* lifetime = file->lifetime;
	*file = {};
	delete lifetime;
}

void releaseMount(SmbDir* directory)
{
	auto* lifetime = directory->lifetime;
	*directory = {};
	delete lifetime;
}

bool retired(const SmbMount* mount)
{
	return !mount || mount->retired.load(std::memory_order_acquire);
}

bool reopenFileUnlocked(SmbFile* file)
{
	if (!file || retired(file->mount) || !file->mount->context)
		return false;
	// Never replay creation-time flags after a connection loss: doing so can
	// truncate a partial copy or make an O_EXCL descriptor unrecoverable.
	const int openFlags=file->openedOnce ? file->flags & ~(O_CREAT|O_EXCL|O_TRUNC) : file->flags;
	file->handle=smb2_open(file->mount->context,file->path,openFlags);
	if(!file->handle) return false;
	file->openedOnce=true;
	if(file->position){
		uint64_t actual=0;
		if(smb2_lseek(file->mount->context,file->handle,static_cast<int64_t>(file->position),SEEK_SET,&actual)<0||actual!=file->position){
			smb2_close(file->mount->context,file->handle); file->handle=nullptr; return false;
		}
	}
	return true;
}

int smbOpen(_reent* reent, void* state, const char* source, int flags, int)
{
	auto mount = mountFrom(reent);
	auto* file = static_cast<SmbFile*>(state);
	std::memset(file, 0, sizeof(*file));
	if (!mount)
		return fail(reent, ENODEV);
	char path[PATH_MAX]{};
	if (!fixPath(source, path, sizeof(path)))
		return fail(reent, ENAMETOOLONG);
	std::lock_guard<std::mutex> lock(mount->ioMutex);
	if (retired(mount.get()) || !pinMount(file, mount))
		return fail(reent, retired(mount.get()) ? ENODEV : ENOMEM);
	file->flags = flags;
	std::snprintf(file->path, sizeof(file->path), "%s", path);
	if (!reopenFileUnlocked(file))
	{
		releaseMount(file);
		return fail(reent, EIO);
	}
	if (flags & O_APPEND)
	{
		uint64_t position = 0;
		const int result = smb2_lseek(mount->context, file->handle, 0, SEEK_END, &position);
		if (result < 0)
		{
			smb2_close(mount->context, file->handle);
			releaseMount(file);
			return fail(reent, -result);
		}
		file->position = position;
	}
	reent->_errno = 0;
	return 0;
}

int smbClose(_reent* reent, void* state)
{
	auto* file = static_cast<SmbFile*>(state);
	auto mount = mountFrom(file);
	if (!mount)
		return fail(reent, EBADF);
	std::lock_guard<std::mutex> lock(mount->ioMutex);
	ReleaseReadAheadBuffer(file);
	const int result = !retired(mount.get()) && file->handle ?
	                   smb2_close(mount->context, file->handle) : 0;
	releaseMount(file);
	if (result < 0)
		return fail(reent, -result);
	reent->_errno = 0;
	return 0;
}

ssize_t smbRead(_reent* reent, void* state, char* output, size_t length)
{
	auto* file = static_cast<SmbFile*>(state);
	auto mount=mountFrom(file);
	if (!mount || !file->handle)
		return fail(reent, EBADF);
	std::lock_guard<std::mutex> lock(mount->ioMutex);
	if(retired(mount.get())) return fail(reent,ENODEV);
	const size_t maximum = std::max<size_t>(1, smb2_get_max_read_size(file->mount->context));
	size_t total = 0;
	if (file->readAheadOffset < file->readAheadSize)
	{
		const size_t cached = std::min(length,
			file->readAheadSize - file->readAheadOffset);
		std::memcpy(output, file->readAhead + file->readAheadOffset, cached);
		file->readAheadOffset += cached;
		file->position += cached;
		total += cached;
		if (file->readAheadOffset == file->readAheadSize)
		{
			file->readAheadOffset = 0;
			file->readAheadSize = 0;
		}
	}
	while (total < length)
	{
		const size_t remaining = length - total;
		const size_t target = std::min(kSmbReadAheadMax,
			kSmbReadAheadMin << std::min(file->sequentialReads, 3u));
		const bool useReadAhead = remaining < target && EnsureReadAheadBuffer(file, target);
		const size_t amount = std::min(useReadAhead ? target : remaining, maximum);
		uint8_t* destination = useReadAhead ? file->readAhead : reinterpret_cast<uint8_t*>(output + total);
		const int result = smb2_read(file->mount->context, file->handle, destination, amount);
		if (result < 0)
			return total ? static_cast<ssize_t>(total) : fail(reent, -result);
		if (result == 0)
			break;
		if (useReadAhead)
		{
			file->sequentialReads = std::min(file->sequentialReads + 1, 3u);
			file->readAheadOffset = 0;
			file->readAheadSize = static_cast<size_t>(result);
			const size_t copied = std::min(remaining, file->readAheadSize);
			std::memcpy(output + total, file->readAhead, copied);
			file->readAheadOffset = copied;
			file->position += copied;
			total += copied;
			if (file->readAheadOffset == file->readAheadSize)
			{
				file->readAheadOffset = 0;
				file->readAheadSize = 0;
			}
			break;
		}
		const size_t bytesRead = static_cast<size_t>(result);
		file->sequentialReads = 0;
		file->position += bytesRead;
		total += bytesRead;
		if (bytesRead < amount)
			break;
	}
	reent->_errno = 0;
	return static_cast<ssize_t>(total);
}

ssize_t smbWrite(_reent* reent, void* state, const char* input, size_t length)
{
	auto* file = static_cast<SmbFile*>(state);
	auto mount=mountFrom(file);
	if (!mount || !file->handle)
		return fail(reent, EBADF);
	std::lock_guard<std::mutex> lock(mount->ioMutex);
	if(retired(mount.get())) return fail(reent,ENODEV);
	const int synchronized = SynchronizeFilePosition(file);
	if (synchronized < 0)
		return fail(reent, -synchronized);
	const size_t maximum = std::max<size_t>(1, smb2_get_max_write_size(file->mount->context));
	size_t total = 0;
	if(length && (file->flags&O_APPEND)){
		uint64_t end=0; const int seek=smb2_lseek(file->mount->context,file->handle,0,SEEK_END,&end);
		if(seek<0) return fail(reent,-seek);
		file->position=end;
	}
	while (total < length)
	{
		const size_t amount = std::min(length - total, maximum);
		const int result = smb2_write(file->mount->context, file->handle,
		                              reinterpret_cast<const uint8_t*>(input + total), amount);
		if (result < 0)
			return total ? static_cast<ssize_t>(total) : fail(reent, -result);
		if (result == 0)
			return total ? static_cast<ssize_t>(total) : fail(reent, EIO);
		total += static_cast<size_t>(result);
		file->position += static_cast<size_t>(result);
	}
	reent->_errno = 0;
	return static_cast<ssize_t>(total);
}

off_t smbSeek(_reent* reent, void* state, off_t position, int origin)
{
	auto* file = static_cast<SmbFile*>(state);
	auto mount=mountFrom(file);
	if (!mount || !file->handle)
	{
		fail(reent, EBADF);
		return static_cast<off_t>(-1);
	}
	std::lock_guard<std::mutex> lock(mount->ioMutex);
	if(retired(mount.get())){fail(reent,ENODEV);return static_cast<off_t>(-1);}
	uint64_t resultPosition = 0;
	if (origin == SEEK_SET || origin == SEEK_CUR)
	{
		if (file->position > static_cast<uint64_t>(LLONG_MAX))
		{
			fail(reent, EOVERFLOW);
			return static_cast<off_t>(-1);
		}
		const int64_t base = origin == SEEK_SET ? 0 : static_cast<int64_t>(file->position);
		if (position > 0 && base > LLONG_MAX - position)
		{
			fail(reent, EOVERFLOW);
			return static_cast<off_t>(-1);
		}
		const int64_t target = base + position;
		if (target < 0)
		{
			fail(reent, EINVAL);
			return static_cast<off_t>(-1);
		}
		if (file->readAheadSize > 0)
		{
			const uint64_t cacheStart = file->position - file->readAheadOffset;
			const uint64_t cacheEnd = cacheStart + file->readAheadSize;
			if (static_cast<uint64_t>(target) >= cacheStart && static_cast<uint64_t>(target) <= cacheEnd)
			{
				file->readAheadOffset = static_cast<size_t>(static_cast<uint64_t>(target) - cacheStart);
				file->position = static_cast<uint64_t>(target);
				reent->_errno = 0;
				return static_cast<off_t>(target);
			}
		}
		const int result = smb2_lseek(file->mount->context, file->handle, target, SEEK_SET, &resultPosition);
		if (result < 0)
		{
			fail(reent, -result);
			return static_cast<off_t>(-1);
		}
	}
	else
	{
		const int result = smb2_lseek(file->mount->context, file->handle, position, origin, &resultPosition);
		if (result < 0)
		{
			fail(reent, -result);
			return static_cast<off_t>(-1);
		}
	}
	if (resultPosition > static_cast<uint64_t>(LLONG_MAX))
	{
		fail(reent, EOVERFLOW);
		return static_cast<off_t>(-1);
	}
	file->readAheadOffset = 0;
	file->readAheadSize = 0;
	file->sequentialReads = 0;
	file->position = resultPosition;
	reent->_errno = 0;
	return static_cast<off_t>(resultPosition);
}

int smbFstat(_reent* reent, void* state, struct stat* output)
{
	auto* file = static_cast<SmbFile*>(state);
	auto mount=mountFrom(file);
	if (!mount || !file->handle || !output)
		return fail(reent, EBADF);
	std::lock_guard<std::mutex> lock(mount->ioMutex);
	if(retired(mount.get())) return fail(reent,ENODEV);
	struct smb2_stat_64 info{};
	const int result = smb2_fstat(file->mount->context, file->handle, &info);
	if (result < 0)
		return fail(reent, -result);
	fillStat(output, info);
	reent->_errno = 0;
	return 0;
}

int smbStat(_reent* reent, const char* source, struct stat* output)
{
	auto mount = mountFrom(reent);
	if (!mount || !output)
		return fail(reent, EINVAL);
	if (isRootPath(source))
	{
		std::memset(output, 0, sizeof(*output));
		output->st_mode = S_IFDIR | 0777;
		output->st_nlink = 1;
		reent->_errno = 0;
		return 0;
	}
	char path[PATH_MAX]{};
	if (!fixPath(source, path, sizeof(path)))
		return fail(reent, ENAMETOOLONG);
	std::lock_guard<std::mutex> lock(mount->ioMutex);
	if (retired(mount.get()) || !mount->context)
		return fail(reent, ENODEV);
	struct smb2_stat_64 info{};
	const int result = smb2_stat(mount->context, path, &info);
	if (result < 0)
		return fail(reent, -result);
	fillStat(output, info);
	reent->_errno = 0;
	return 0;
}

template <typename Operation>
int pathOperation(_reent* reent, const char* source, Operation operation)
{
	auto mount = mountFrom(reent);
	if (!mount)
		return fail(reent, ENODEV);
	char path[PATH_MAX]{};
	if (!fixPath(source, path, sizeof(path)))
		return fail(reent, ENAMETOOLONG);
	std::lock_guard<std::mutex> lock(mount->ioMutex);
	if (retired(mount.get()) || !mount->context)
		return fail(reent, ENODEV);
	const int result = operation(mount.get(), path);
	if (result < 0)
		return fail(reent, -result);
	reent->_errno = 0;
	return 0;
}

int smbUnlink(_reent* reent, const char* path)
{
	return pathOperation(reent, path, [](SmbMount* mount, const char* fixed) {
		return smb2_unlink(mount->context, fixed);
	});
}

int smbMkdir(_reent* reent, const char* path, int)
{
	return pathOperation(reent, path, [](SmbMount* mount, const char* fixed) {
		return smb2_mkdir(mount->context, fixed);
	});
}

int smbRmdir(_reent* reent, const char* path)
{
	return pathOperation(reent, path, [](SmbMount* mount, const char* fixed) {
		return smb2_rmdir(mount->context, fixed);
	});
}

int smbRename(_reent* reent, const char* source, const char* destination)
{
	auto mount = mountFrom(reent);
	if (!mount)
		return fail(reent, ENODEV);
	char oldPath[PATH_MAX]{}, newPath[PATH_MAX]{};
	if (!fixPath(source, oldPath, sizeof(oldPath)) ||
	    !fixPath(destination, newPath, sizeof(newPath)))
		return fail(reent, ENAMETOOLONG);
	std::lock_guard<std::mutex> lock(mount->ioMutex);
	if (retired(mount.get()) || !mount->context)
		return fail(reent, ENODEV);
	const int result = smb2_rename(mount->context, oldPath, newPath);
	if (result < 0)
		return fail(reent, -result);
	reent->_errno = 0;
	return 0;
}

DIR_ITER* smbDirOpen(_reent* reent, DIR_ITER* state, const char* source)
{
	auto mount = mountFrom(reent);
	auto* directory = state ? static_cast<SmbDir*>(state->dirStruct) : nullptr;
	if (!mount || !directory)
	{
		fail(reent, EINVAL);
		return nullptr;
	}
	std::memset(directory, 0, sizeof(*directory));
	char path[PATH_MAX]{};
	if (!fixPath(source, path, sizeof(path)))
	{
		fail(reent, ENAMETOOLONG);
		return nullptr;
	}
	std::lock_guard<std::mutex> lock(mount->ioMutex);
	if (retired(mount.get()) || !pinMount(directory, mount))
	{
		fail(reent, retired(mount.get()) ? ENODEV : ENOMEM);
		return nullptr;
	}
	directory->handle = smb2_opendir(mount->context, path);
	if (!directory->handle)
	{
		releaseMount(directory);
		fail(reent, EIO);
		return nullptr;
	}
	directory->entries = new (std::nothrow) std::vector<SmbDir::CachedEntry>();
	if (!directory->entries)
	{
		smb2_closedir(mount->context, directory->handle);
		releaseMount(directory);
		fail(reent, ENOMEM);
		return nullptr;
	}
	while (const struct smb2dirent* entry = smb2_readdir(mount->context, directory->handle))
	{
		SmbDir::CachedEntry cached{};
		std::snprintf(cached.name, sizeof(cached.name), "%s", entry->name);
		fillStat(&cached.info, entry->st);
		directory->entries->push_back(cached);
		std::string child(path);
		if (!child.empty()) child += '/';
		child += entry->name;
		mount->directoryMetadata[std::move(child)] = cached.info;
	}
	directory->index = 0;
	reent->_errno = 0;
	return state;
}

int smbDirReset(_reent* reent, DIR_ITER* state)
{
	auto* directory = state ? static_cast<SmbDir*>(state->dirStruct) : nullptr;
	auto mount=mountFrom(directory);
	if (!mount || !directory->handle || !directory->entries)
		return fail(reent, EBADF);
	if(retired(mount.get())) return fail(reent,ENODEV);
	directory->index = 0;
	reent->_errno = 0;
	return 0;
}

int smbDirNext(_reent* reent, DIR_ITER* state, char* name, struct stat* output)
{
	auto* directory = state ? static_cast<SmbDir*>(state->dirStruct) : nullptr;
	auto mount=mountFrom(directory);
	if (!mount || !directory->handle || !directory->entries || !name || !output)
		return fail(reent, EBADF);
	if(retired(mount.get())) return fail(reent,ENODEV);
	if (directory->index >= directory->entries->size())
		return fail(reent, ENOENT);
	const SmbDir::CachedEntry& entry = (*directory->entries)[directory->index++];
	std::snprintf(name, NAME_MAX, "%s", entry.name);
	*output = entry.info;
	reent->_errno = 0;
	return 0;
}

int smbDirClose(_reent* reent, DIR_ITER* state)
{
	auto* directory = state ? static_cast<SmbDir*>(state->dirStruct) : nullptr;
	auto mount = mountFrom(directory);
	if (!mount)
		return fail(reent, EBADF);
	std::lock_guard<std::mutex> lock(mount->ioMutex);
	if (!retired(mount.get()) && directory->handle)
		smb2_closedir(mount->context, directory->handle);
	delete directory->entries;
	releaseMount(directory);
	reent->_errno = 0;
	return 0;
}

int smbStatvfs(_reent* reent, const char* source, struct statvfs* output)
{
	auto mount = mountFrom(reent);
	if (!mount || !output)
		return fail(reent, EINVAL);
	char path[PATH_MAX]{};
	if (!fixPath(source, path, sizeof(path)))
		return fail(reent, ENAMETOOLONG);
	std::lock_guard<std::mutex> lock(mount->ioMutex);
	if (retired(mount.get()) || !mount->context)
		return fail(reent, ENODEV);
	struct smb2_statvfs info{};
	const int result = smb2_statvfs(mount->context, path, &info);
	if (result < 0)
		return fail(reent, -result);
	std::memset(output, 0, sizeof(*output));
	output->f_bsize = info.f_bsize;
	output->f_frsize = info.f_frsize;
	output->f_blocks = info.f_blocks;
	output->f_bfree = info.f_bfree;
	output->f_bavail = info.f_bavail;
	output->f_files = info.f_files;
	output->f_ffree = info.f_ffree;
	output->f_favail = info.f_favail;
	output->f_fsid = info.f_fsid;
	output->f_flag = info.f_flag;
	output->f_namemax = info.f_namemax;
	reent->_errno = 0;
	return 0;
}

int smbTruncate(_reent* reent, void* state, off_t length)
{
	auto* file = static_cast<SmbFile*>(state);
	auto mount=mountFrom(file);
	if (!mount || !file->handle)
		return fail(reent, EBADF);
	std::lock_guard<std::mutex> lock(mount->ioMutex);
	if(retired(mount.get())) return fail(reent,ENODEV);
	const int synchronized = SynchronizeFilePosition(file);
	if (synchronized < 0)
		return fail(reent, -synchronized);
	const int result = smb2_ftruncate(file->mount->context, file->handle, length);
	if (result < 0)
		return fail(reent, -result);
	reent->_errno = 0;
	return 0;
}

int smbSync(_reent* reent, void* state)
{
	auto* file = static_cast<SmbFile*>(state);
	auto mount=mountFrom(file);
	if (!mount || !file->handle)
		return fail(reent, EBADF);
	std::lock_guard<std::mutex> lock(mount->ioMutex);
	if(retired(mount.get())) return fail(reent,ENODEV);
	const int result = smb2_fsync(file->mount->context, file->handle);
	if (result < 0)
		return fail(reent, -result);
	reent->_errno = 0;
	return 0;
}

std::unordered_map<std::string, std::string> readIni(const std::string& path)
{
	std::unordered_map<std::string, std::string> values;
	FILE* file = std::fopen(path.c_str(), "rb");
	if (!file)
		return values;
	char line[4096];
	while (std::fgets(line, sizeof(line), file))
	{
		std::string text = trim(line);
		if (text.empty() || text.front() == '#' || text.front() == ';' || text.front() == '[')
			continue;
		const auto separator = text.find('=');
		if (separator == std::string::npos)
			continue;
		std::string key = trim(text.substr(0, separator));
		if (!key.empty())
			values[key] = trim(text.substr(separator + 1));
	}
	std::fclose(file);
	return values;
}

std::string valueFor(const std::unordered_map<std::string, std::string>& values,
	                 const std::string& key)
{
	const auto iterator = values.find(key);
	return iterator == values.end() ? std::string{} : iterator->second;
}
} // namespace

std::string SmbRootPath(const std::string& id)
{
	const std::string deviceName = deviceNameForId(id);
	return deviceName.empty() ? std::string{} : deviceName + ":/";
}

std::string SmbBrowsePath(const SmbShare& share)
{
	std::string result = SmbRootPath(share.id);
	if (result.empty() || share.path.empty())
		return result;
	std::string path = share.path;
	std::replace(path.begin(), path.end(), '\\', '/');
	while (!path.empty() && path.front() == '/')
		path.erase(path.begin());
	while (!path.empty() && path.back() == '/')
		path.pop_back();
	return path.empty() ? result : result + path;
}

bool InitializeUsb(std::string* error)
{
	std::lock_guard<std::mutex> initLock(s_usbInitMutex);
	if (s_usbInitialized.load(std::memory_order_acquire)) return true;

	// Register before starting the libusbhsfs manager. Registering afterwards
	// misses the first populate event, so a drive already attached at boot is
	// never published until it is unplugged and reconnected.
	usbHsFsSetFileSystemMountFlags(UsbHsFsMountFlags_ReplayJournal | UsbHsFsMountFlags_ShowHiddenFiles);
	usbHsFsSetPopulateCallback(usbStatusChanged,nullptr);
	const Result result=usbHsFsInitialize(0);
	if (R_FAILED(result))
	{
		usbHsFsSetPopulateCallback(nullptr,nullptr);
		if (error) { char message[64]; std::snprintf(message,sizeof(message),"USB initialization failed (0x%08x)",result); *error=message; }
		return false;
	}

	// Device discovery continues on libusbhsfs' manager thread. The callback
	// publishes the first and all subsequent snapshots without blocking the UI.
	s_usbInitialized.store(true,std::memory_order_release);
	return true;
}

uint64_t UsbStatusGeneration()
{
	return s_usbGeneration.load(std::memory_order_acquire);
}

void SetUsbStatusCallback(UsbStatusCallback callback, void* userData)
{
	std::lock_guard<std::mutex> lock(s_usbCallbackMutex);
	s_usbCallback=callback;
	s_usbCallbackData=callback?userData:nullptr;
}

UsbSnapshot GetUsbSnapshot()
{
	UsbSnapshot snapshot;
	if (!s_usbInitialized.load(std::memory_order_acquire)) return snapshot;
	std::lock_guard<std::mutex> lock(s_usbMutex);
	snapshot.generation=s_usbGeneration.load(std::memory_order_acquire);
	for(const auto& device:s_usbDevices)
		if(device.name[0]) snapshot.locations.emplace_back(makeUsbLocation(device));
	return snapshot;
}

std::string ResolveUsbPath(const std::string& id)
{
	for(const auto& location:GetUsbSnapshot().locations)
		if(location.id==id) return location.path;
	return {};
}

bool SafelyEjectUsb(const std::string& id, std::string* error)
{
	UsbHsFsDevice target{}; bool found=false; { std::lock_guard<std::mutex> lock(s_usbMutex); for(const auto& device:s_usbDevices) if(usbVolumeId(device)==id||usbPhysicalId(device)==id){ target=device; found=true; break; } }
	if(!found){ if(error)*error="The USB drive is no longer connected"; return false; }
	if(!usbHsFsUnmountDevice(&target,true)){ if(error)*error="Could not safely eject the USB drive; close files using it and try again"; return false; }
	return true;
}

bool GetCachedSmbStat(const std::string& path, struct stat* output)
{
	if (!output) return false;
	std::lock_guard<std::mutex> mountsLock(s_mountMutex);
	for (const auto& mount : s_smbMounts)
	{
		if (!mount->device || path.rfind(mount->device->rootPath, 0) != 0) continue;
		char fixed[PATH_MAX]{};
		if (!fixPath(path.c_str(), fixed, sizeof(fixed))) return false;
		std::lock_guard<std::mutex> ioLock(mount->ioMutex);
		const auto found = mount->directoryMetadata.find(fixed);
		if (found == mount->directoryMetadata.end()) return false;
		*output = found->second;
		return true;
	}
	return false;
}

bool MountSmb(const SmbShare& share, std::string* error, const std::atomic_bool* cancel)
{
	if (!validId(share.id) || share.server.empty() || share.share.empty())
	{
		if (error)
			*error = "SMB share settings are incomplete";
		return false;
	}
	{ std::lock_guard<std::mutex> lock(s_mountMutex); for(const auto& mount:s_smbMounts) if(mount->config.id==share.id) return true; }
	if(cancel&&cancel->load(std::memory_order_relaxed)){ if(error)*error="Connection cancelled"; return false; }

	auto mount = std::make_shared<SmbMount>();
	mount->config = share;
	mount->state = SmbConnectionState::Connecting;
	mount->context = smb2_init_context();
	if (!mount->context)
	{
		if (error)
			*error = "Could not create the SMB client";
		return false;
	}
	smb2_set_security_mode(mount->context, SMB2_NEGOTIATE_SIGNING_ENABLED);
	smb2_set_timeout(mount->context, 6);
	if (!share.user.empty())
		smb2_set_user(mount->context, share.user.c_str());
	if (!share.password.empty())
		smb2_set_password(mount->context, share.password.c_str());
	if (!share.domain.empty())
		smb2_set_domain(mount->context, share.domain.c_str());
	const int connected = smb2_connect_share(mount->context, share.server.c_str(),
	                                         share.share.c_str(),
	                                         share.user.empty() ? nullptr : share.user.c_str());
	if (connected < 0)
	{
		if (error)
		{
			const char* detail = smb2_get_error(mount->context);
			*error = detail && *detail ? detail : "Could not connect to the SMB share";
		}
		mount->state = SmbConnectionState::Failed;
		return false;
	}
	if(cancel&&cancel->load(std::memory_order_relaxed)){ if(error)*error="Connection cancelled"; return false; }
	mount->connected = true;
	mount->state = SmbConnectionState::Connected;

	std::lock_guard<std::mutex> lock(s_mountMutex);
	for(const auto& existing:s_smbMounts) if(existing->config.id==share.id) return true;
	auto device=std::make_unique<SmbDevice>();
	device->deviceName=deviceNameForId(share.id); device->rootPath=device->deviceName+":/"; device->mount=mount;
	device->devoptab.name=device->deviceName.c_str(); device->devoptab.structSize=sizeof(SmbFile);
	device->devoptab.open_r=smbOpen; device->devoptab.close_r=smbClose;
	device->devoptab.write_r=smbWrite; device->devoptab.read_r=smbRead;
	device->devoptab.seek_r=smbSeek; device->devoptab.fstat_r=smbFstat;
	device->devoptab.stat_r=smbStat; device->devoptab.unlink_r=smbUnlink;
	device->devoptab.rename_r=smbRename; device->devoptab.mkdir_r=smbMkdir;
	device->devoptab.dirStateSize=sizeof(SmbDir); device->devoptab.diropen_r=smbDirOpen;
	device->devoptab.dirreset_r=smbDirReset; device->devoptab.dirnext_r=smbDirNext;
	device->devoptab.dirclose_r=smbDirClose; device->devoptab.statvfs_r=smbStatvfs;
	device->devoptab.ftruncate_r=smbTruncate; device->devoptab.fsync_r=smbSync;
	device->devoptab.deviceData=device.get(); device->devoptab.rmdir_r=smbRmdir; device->devoptab.lstat_r=smbStat;
	mount->device=device.get();
	if (AddDevice(&device->devoptab) < 0)
	{
		if (error)
			*error = "No free filesystem slot is available for the SMB share";
		return false;
	}
	s_smbDevices.emplace_back(std::move(device));
	s_smbMounts.emplace_back(std::move(mount));
	return true;
}

bool UnmountSmb(const std::string& id)
{
	std::shared_ptr<SmbMount> removed;
	{
		std::lock_guard<std::mutex> lock(s_mountMutex);
		const auto iterator=std::find_if(s_smbMounts.begin(),s_smbMounts.end(),[&](const auto& mount){return mount->config.id==id;});
		if(iterator==s_smbMounts.end()) return true;
		(*iterator)->retired.store(true,std::memory_order_release);
		(*iterator)->state.store(SmbConnectionState::Disconnected,std::memory_order_release);
		if((*iterator)->device){ (*iterator)->device->mount.reset(); RemoveDevice((*iterator)->device->rootPath.c_str()); }
		removed=std::move(*iterator); s_smbMounts.erase(iterator);
	}
	std::lock_guard<std::mutex> ioLock(removed->ioMutex);
	if(removed->context){ if(removed->connected)smb2_disconnect_share(removed->context); smb2_destroy_context(removed->context); removed->context=nullptr; removed->connected=false; }
	return true;
}

bool IsSmbMounted(const std::string& id)
{
	std::lock_guard<std::mutex> lock(s_mountMutex);
	return std::any_of(s_smbMounts.begin(), s_smbMounts.end(),
	                   [&](const auto& mount) { return mount->config.id == id; });
}

SmbConnectionState GetSmbConnectionState(const std::string& id)
{
	std::lock_guard<std::mutex> lock(s_mountMutex); for(const auto& mount:s_smbMounts) if(mount->config.id==id) return mount->state.load(std::memory_order_acquire); return SmbConnectionState::Disconnected;
}

bool ReconnectSmb(const std::string& id, std::string* error, const std::atomic_bool* cancel)
{
	SmbShare config; { std::lock_guard<std::mutex> lock(s_mountMutex); const auto it=std::find_if(s_smbMounts.begin(),s_smbMounts.end(),[&](const auto& mount){return mount->config.id==id;}); if(it==s_smbMounts.end()){if(error)*error="SMB share is not registered";return false;} config=(*it)->config; }
	UnmountSmb(id); return MountSmb(config,error,cancel);
}

std::vector<Location> ListUsbLocations()
{
	return GetUsbSnapshot().locations;
}

std::vector<SmbShare> LoadSmbShares(const std::string& iniPath)
{
	const auto values = readIni(iniPath);
	const std::string countText = valueFor(values, "Storage/SmbCount");
	const int count = std::clamp(std::atoi(countText.c_str()), 0, 8);
	std::vector<SmbShare> shares;
	shares.reserve(count);
	for (int index = 0; index < count; ++index)
	{
		const std::string prefix = "Storage/Smb" + std::to_string(index);
		SmbShare share;
		share.id = valueFor(values, prefix + "Id");
		share.name = valueFor(values, prefix + "Name");
		share.server = valueFor(values, prefix + "Server");
		share.share = valueFor(values, prefix + "Share");
		share.path = valueFor(values, prefix + "Path");
		share.user = valueFor(values, prefix + "User");
		share.password = valueFor(values, prefix + "Password");
		share.domain = valueFor(values, prefix + "Domain");
		const std::string automatic = valueFor(values, prefix + "AutoMount");
		share.autoMount = automatic.empty() || automatic == "1" || automatic == "true";
		if (validId(share.id) && !share.server.empty() && !share.share.empty())
			shares.emplace_back(std::move(share));
	}
	return shares;
}

void InitializeFromConfig(const std::string& iniPath, bool initializeUsb,
                           std::vector<std::string>* errors,
                           const std::string& requiredPath)
{
	std::string error;
	if (initializeUsb && !InitializeUsb(&error) && errors)
		errors->emplace_back(std::move(error));
	for (const auto& share : LoadSmbShares(iniPath))
	{
		const std::string root = SmbRootPath(share.id);
		const bool required = !requiredPath.empty() && !root.empty() &&
			requiredPath.compare(0, root.size(), root) == 0;
		if (!share.autoMount && !required)
			continue;
		error.clear();
		if (!MountSmb(share, &error) && errors)
			errors->emplace_back((share.name.empty() ? share.share : share.name) + ": " + error);
	}
}

void Shutdown()
{
	SetUsbStatusCallback(nullptr,nullptr);
	std::vector<std::shared_ptr<SmbMount>> mounts;
	{
		std::lock_guard<std::mutex> lock(s_mountMutex);
		for(auto& mount:s_smbMounts){
			mount->retired.store(true,std::memory_order_release);
			mount->state.store(SmbConnectionState::Disconnected,std::memory_order_release);
			if(mount->device){mount->device->mount.reset();RemoveDevice(mount->device->rootPath.c_str());}
		}
		mounts.swap(s_smbMounts);
	}
	for(const auto& mount:mounts){
		std::lock_guard<std::mutex> ioLock(mount->ioMutex);
		if(mount->context){if(mount->connected)smb2_disconnect_share(mount->context);smb2_destroy_context(mount->context);mount->context=nullptr;mount->connected=false;}
	}
	if (s_usbInitialized)
	{
		usbHsFsSetPopulateCallback(nullptr, nullptr);
		usbHsFsExit();
		s_usbInitialized = false;
	}
}
} // namespace SwitchStorage
