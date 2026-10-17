#include "ExceptionHandler.h"

#include <switch.h>
#include <switch/nro.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

#define CEMU_STRINGIZE_IMPL(value) #value
#define CEMU_STRINGIZE(value) CEMU_STRINGIZE_IMPL(value)

namespace
{
	std::atomic_flag s_handlingException = ATOMIC_FLAG_INIT;
	char s_imageId[65]{};
	u64 s_processId = 0;
#if defined(ENABLE_DEKO3D)
	constexpr const char* s_coreName = "Deko3D";
#else
	constexpr const char* s_coreName = "Vulkan/OpenGL";
#endif

	void WriteAll(int fd, const char* data, size_t size)
	{
		while (size != 0)
		{
			const ssize_t written = ::write(fd, data, size);
			if (written > 0)
			{
				data += written;
				size -= static_cast<size_t>(written);
				continue;
			}
			if (written < 0 && errno == EINTR)
				continue;
			return;
		}
	}
}

extern "C"
{
	void _start() __attribute__((visibility("hidden")));
	alignas(16) u8 __nx_exception_stack[0x10000];
	u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

	void __libnx_exception_handler(ThreadExceptionDump* context)
	{
		if (s_handlingException.test_and_set(std::memory_order_relaxed))
			return;

		int fd = ::open("sdmc:/switch/cemu/crash.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (fd < 0)
			fd = ::open("crash.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (fd < 0)
			return;

		char report[4096]{};
		size_t used = 0;
		auto append = [&](const char* format, auto... args) {
			if (used >= sizeof(report))
				return;
			const int length = std::snprintf(report + used, sizeof(report) - used, format, args...);
			if (length <= 0)
				return;
			used += std::min(static_cast<size_t>(length), sizeof(report) - used - 1);
		};

		append("Cemu Switch native exception\n");
#if defined(EMULATOR_HASH)
		append("build: %s\n", CEMU_STRINGIZE(EMULATOR_HASH));
#endif
		append("core: %s\n", s_coreName);
		append("image_id: %s\n", s_imageId);
		append("process_id: 0x%llx\n", static_cast<unsigned long long>(s_processId));
		append("module_base: 0x%016llx\n", reinterpret_cast<unsigned long long>(&_start));
		append("error_desc: 0x%08x\n", context->error_desc);
		for (int index = 0; index < 29; ++index)
			append("x%-2d: 0x%016llx\n", index,
				static_cast<unsigned long long>(context->cpu_gprs[index].x));
		append("fp: 0x%016llx\n", static_cast<unsigned long long>(context->fp.x));
		append("lr: 0x%016llx\n", static_cast<unsigned long long>(context->lr.x));
		append("sp: 0x%016llx\n", static_cast<unsigned long long>(context->sp.x));
		append("pc: 0x%016llx\n", static_cast<unsigned long long>(context->pc.x));
		append("pstate: 0x%08x\n", context->pstate);
		append("afsr0: 0x%08x\n", context->afsr0);
		append("afsr1: 0x%08x\n", context->afsr1);
		append("esr: 0x%08x\n", context->esr);
		append("far: 0x%016llx\n", static_cast<unsigned long long>(context->far.x));
		MemoryInfo mapping{};
		u32 pageInfo = 0;
		const Result query = svcQueryMemory(&mapping, &pageInfo, context->far.x);
		if (R_SUCCEEDED(query))
			append("far_mapping: 0x%llx+0x%llx type=0x%x perm=0x%x attr=0x%x device_refs=%u ipc_refs=%u\n",
				static_cast<unsigned long long>(mapping.addr), static_cast<unsigned long long>(mapping.size),
				mapping.type, mapping.perm, mapping.attr, mapping.device_refcount, mapping.ipc_refcount);
		else
			append("far_mapping_query: 0x%08x\n", query);
		WriteAll(fd, report, used);
		::close(fd);
	}
}

void ExceptionHandler_Init()
{
	const auto* header = reinterpret_cast<const NroHeader*>(reinterpret_cast<const u8*>(&_start) + sizeof(NroStart));
	if (header->magic == NROHEADER_MAGIC)
		for (size_t i = 0; i < sizeof(header->build_id); ++i)
			std::snprintf(s_imageId + i * 2, 3, "%02x", header->build_id[i]);
	svcGetProcessId(&s_processId, CUR_PROCESS_HANDLE);
}

#undef CEMU_STRINGIZE
#undef CEMU_STRINGIZE_IMPL
