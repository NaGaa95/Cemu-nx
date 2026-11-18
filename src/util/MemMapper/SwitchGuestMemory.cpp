#include "util/MemMapper/SwitchGuestMemory.h"

#include <switch.h>
#include <cstdio>
#include <type_traits>

namespace MemMapper::SwitchGuestMemory
{
	using namespace SwitchGuestMemoryLayout;

	namespace
	{
		struct StackBlock
		{
			uintptr_t address, size;
			u32 type, permission, attribute;
			bool mainStack;
		};

		struct StackSnapshot
		{
			Range region;
			uintptr_t largestFree, largestAt;
			StackBlock blocks[64];
			size_t count, omitted;
			Result result;
		};

		// These objects must survive writes made before __libc_init_array.
		constinit Reservation s_pending{};
		constinit StackSnapshot s_beforeServices{};
		constinit bool s_attempted = false;
		constinit char s_note[288] = "early reservation not attempted";
		static_assert(std::is_trivial_v<Reservation> && std::is_trivial_v<StackSnapshot>);

		Result getRange(InfoType addressInfo, InfoType sizeInfo, Range& range)
		{
			u64 address = 0, size = 0;
			Result rc = svcGetInfo(&address, addressInfo, CUR_PROCESS_HANDLE, 0);
			if (R_FAILED(rc))
				return rc;
			rc = svcGetInfo(&size, sizeInfo, CUR_PROCESS_HANDLE, 0);
			if (R_FAILED(rc))
				return rc;
			if (size == 0 || size > UINTPTR_MAX - address)
				return KERNELRESULT(InvalidSize);
			range = {address, address + size};
			return 0;
		}

		Result query(uintptr_t address, MemoryInfo& info)
		{
			u32 pageInfo = 0;
			Result rc = svcQueryMemory(&info, &pageInfo, address);
			if (R_FAILED(rc))
				return rc;
			if (info.addr > address || info.size > UINTPTR_MAX - info.addr || info.addr + info.size <= address)
				return KERNELRESULT(InvalidSize);
			return 0;
		}

		void captureStack(StackSnapshot& snapshot)
		{
			snapshot = {};
			snapshot.result = getRange(InfoType_StackRegionAddress, InfoType_StackRegionSize, snapshot.region);
			if (R_FAILED(snapshot.result))
				return;
			const char stackMarker = 0;
			const uintptr_t currentStack = reinterpret_cast<uintptr_t>(&stackMarker);
			for (uintptr_t address = snapshot.region.begin; address < snapshot.region.end;)
			{
				MemoryInfo info{};
				snapshot.result = query(address, info);
				if (R_FAILED(snapshot.result))
					return;
				const uintptr_t end = std::min<uintptr_t>(info.addr + info.size, snapshot.region.end);
				if ((info.type & 0xFF) == MemType_Unmapped)
				{
					if (end - address > snapshot.largestFree)
					{
						snapshot.largestFree = end - address;
						snapshot.largestAt = address;
					}
				}
				else if (snapshot.count < sizeof(snapshot.blocks) / sizeof(StackBlock))
				{
					snapshot.blocks[snapshot.count++] = {address, end - address, info.type, info.perm, info.attr,
						currentStack >= address && currentStack < end};
				}
				else
					++snapshot.omitted;
				address = end;
			}
		}

		void releaseLocked(Reservation& reservation)
		{
			for (VirtmemReservation* handle : reservation.handles)
				if (handle)
					virtmemRemoveReservation(handle);
			reservation = {};
		}

		void printSnapshot(const char* phase, const StackSnapshot& snapshot, void (*writeLine)(void*, const char*), void* context)
		{
			char line[256];
			std::snprintf(line, sizeof(line), "Switch: %s stack 0x%llx+%lluMB, largest kernel-unmapped span %lluKB at 0x%llx, query rc=0x%08x",
				phase, (unsigned long long)snapshot.region.begin, (unsigned long long)((snapshot.region.end - snapshot.region.begin) >> 20),
				(unsigned long long)(snapshot.largestFree >> 10), (unsigned long long)snapshot.largestAt, (unsigned)snapshot.result);
			writeLine(context, line);
			for (size_t i = 0; i < snapshot.count; ++i)
			{
				const StackBlock& block = snapshot.blocks[i];
				std::snprintf(line, sizeof(line), "Switch: %s occupied 0x%llx+0x%llx type 0x%02x perm 0x%x attr 0x%x%s",
					phase, (unsigned long long)block.address, (unsigned long long)block.size,
					(unsigned)block.type, (unsigned)block.permission, (unsigned)block.attribute, block.mainStack ? " (main stack)" : "");
				writeLine(context, line);
			}
			if (snapshot.omitted)
			{
				std::snprintf(line, sizeof(line), "Switch: %s omitted %zu additional occupied blocks", phase, snapshot.omitted);
				writeLine(context, line);
			}
		}
	}

	void ReserveEarly()
	{
		if (s_attempted)
			return;
		s_attempted = true;
#if defined(__SWITCH2__)
		// The Switch 2 build backs the guest window with one flat block (MemMapperSwitch.cpp): no MEM2 window has to
		// be found in the stack region before services start.
		std::snprintf(s_note, sizeof(s_note), "Switch 2: flat guest window, no early reservation");
		return;
#endif
		virtmemLock();
		captureStack(s_beforeServices);
		Range aslr{}, heap{}, alias{};
		Result rc = s_beforeServices.result;
		if (R_SUCCEEDED(rc)) rc = getRange(InfoType_AslrRegionAddress, InfoType_AslrRegionSize, aslr);
		if (R_SUCCEEDED(rc)) rc = getRange(InfoType_HeapRegionAddress, InfoType_HeapRegionSize, heap);
		if (R_SUCCEEDED(rc)) rc = getRange(InfoType_AliasRegionAddress, InfoType_AliasRegionSize, alias);
		if (R_FAILED(rc))
		{
			std::snprintf(s_note, sizeof(s_note), "early address-region query failed (rc=0x%08x)", (unsigned)rc);
			virtmemUnlock();
			return;
		}
		size_t blockedRange = 0;
		uintptr_t blockedAt = 0;
		const uintptr_t base = FindBase(s_beforeServices.region, aslr, [&](uintptr_t address, uintptr_t& end, bool& free) {
			MemoryInfo info{};
			rc = query(address, info);
			if (R_FAILED(rc))
				return false;
			end = info.addr + info.size;
			free = (info.type & 0xFF) == MemType_Unmapped;
			for (const Range& excluded : {heap, alias})
			{
				if (address >= excluded.begin && address < excluded.end)
				{
					free = false;
					end = std::min(end, excluded.end);
				}
				else if (address < excluded.begin)
					end = std::min(end, excluded.begin);
			}
			return true;
		}, &blockedRange, &blockedAt);
		if (!base)
		{
			std::snprintf(s_note, sizeof(s_note),
				"no compatible 1024MB MEM2 window before services (query rc=0x%08x, "
				"blocked in range %zu at 0x%llx; heap 0x%llx..0x%llx alias 0x%llx..0x%llx)",
				(unsigned)rc, blockedRange, (unsigned long long)blockedAt,
				(unsigned long long)heap.begin, (unsigned long long)heap.end,
				(unsigned long long)alias.begin, (unsigned long long)alias.end);
			virtmemUnlock();
			return;
		}
		s_pending.base = reinterpret_cast<void*>(base);
		s_pending.size = GuestSize;
		for (size_t i = 0; i < ReservationCount; ++i)
		{
			const Range& range = MappedRanges[i];
			s_pending.handles[i] = virtmemAddReservation(reinterpret_cast<void*>(base + range.begin), range.end - range.begin);
			if (!s_pending.handles[i])
			{
				releaseLocked(s_pending);
				std::snprintf(s_note, sizeof(s_note), "early reservation bookkeeping allocation failed at range %zu", i);
				virtmemUnlock();
				return;
			}
		}
		std::snprintf(s_note, sizeof(s_note), "reserved before services: guest base 0x%llx, MEM2 0x%llx+1024MB (%zu ranges; gaps available for threads)",
			(unsigned long long)base, (unsigned long long)(base + Mem2Begin), ReservationCount);
		virtmemUnlock();
	}

	Reservation TakeEarly()
	{
		virtmemLock();
		if (!s_pending.base)
		{
			virtmemUnlock();
			return {};
		}
		const uintptr_t base = reinterpret_cast<uintptr_t>(s_pending.base);
		for (const Range& range : MappedRanges)
		{
			for (uintptr_t address = base + range.begin; address < base + range.end;)
			{
				MemoryInfo info{};
				const Result rc = query(address, info);
				if (R_FAILED(rc) || (info.type & 0xFF) != MemType_Unmapped)
				{
					std::snprintf(s_note, sizeof(s_note), "early window unavailable at adoption: 0x%llx type 0x%x rc=0x%08x",
						(unsigned long long)address, (unsigned)info.type, (unsigned)rc);
					releaseLocked(s_pending);
					virtmemUnlock();
					return {};
				}
				address = info.addr + info.size;
			}
		}
		Reservation reservation = s_pending;
		s_pending = {};
		std::snprintf(s_note, sizeof(s_note), "adopted early reservation: guest base 0x%llx, MEM2 0x%llx+1024MB",
			(unsigned long long)base, (unsigned long long)(base + Mem2Begin));
		virtmemUnlock();
		return reservation;
	}

	void Release(Reservation& reservation)
	{
		virtmemLock();
		releaseLocked(reservation);
		virtmemUnlock();
	}

	void DiscardEarly(const char* reason)
	{
		virtmemLock();
		if (s_pending.base)
		{
			releaseLocked(s_pending);
			std::snprintf(s_note, sizeof(s_note), "early reservation released: %s", reason);
		}
		virtmemUnlock();
	}

	const char* DescribeEarlyReservation()
	{
		return s_note;
	}

	void PrintDiagnostics(void (*writeLine)(void*, const char*), void* context)
	{
		StackSnapshot current{};
		virtmemLock();
		captureStack(current);
		virtmemUnlock();
		char line[256];
		std::snprintf(line, sizeof(line), "Switch: %s", s_note);
		writeLine(context, line);
		printSnapshot("before services", s_beforeServices, writeLine, context);
		printSnapshot("after services", current, writeLine, context);
	}
}
