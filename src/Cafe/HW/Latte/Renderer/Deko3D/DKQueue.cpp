#include "DKCommon.h"
#include "DKCommandBufferManager.h"
#include "DKMemoryTracker.h"
#include <switch.h>

extern "C" Result __real_nvGpuChannelKickoff(NvGpuChannel* channel);
extern "C" Result __real_nvGpuChannelAppendEntry(NvGpuChannel* channel, iova_t address, size_t commands, u32 flags, u32 threshold);

namespace
{
	Result SubmitChannel(NvGpuChannel* channel)
	{
		constexpr Result timeout = MAKERESULT(Module_LibnxNvidia, LibnxNvidiaError_Timeout);
		Result result = __real_nvGpuChannelKickoff(channel);
		if (result != timeout || !channel->num_entries)
			return result;

		NvFence acceptedFence = channel->fence;
		const auto healthy = [channel] {
			NvNotification notification{};
			return R_SUCCEEDED(nvioctlChannel_GetErrorNotification(channel->base.fd, &notification)) && !notification.status;
		};
		const u64 start = armGetSystemTick();
		u32 retries = 0;
		while (armTicksToNs(armGetSystemTick() - start) < 10'000'000'000ULL)
		{
			if (!healthy())
				break;

			const Result waitResult = nvFenceWait(&acceptedFence, 100'000);
			if (waitResult == timeout)
				continue;
			if (R_FAILED(waitResult))
				break;

			if (armTicksToNs(armGetSystemTick() - start) >= 10'000'000'000ULL || !healthy())
				break;
			++retries;
			result = __real_nvGpuChannelKickoff(channel);
			if (R_SUCCEEDED(result))
			{
				return result;
			}
			if (result != timeout)
				break;
			svcSleepThread(1'000'000);
		}
		if (armTicksToNs(armGetSystemTick() - start) >= 10'000'000'000ULL)
			cemuLog_log(LogType::Force, "Deko3D submission recovery timed out: {} retries, accepted syncpoint {}:{}",
				retries, acceptedFence.id, acceptedFence.value);
		return result;
	}
}

extern "C" Result __wrap_nvGpuChannelKickoff(NvGpuChannel* channel)
{
	const Result result = SubmitChannel(channel);
	if (R_SUCCEEDED(result))
		return result;

	cemuLog_log(LogType::Force,
		"Deko3D submission failed: result 0x{:08x} ({}-{}), channel {}, entries {}, syncpoint {}:{}+{}, fence {}, memory {} MB",
		result, R_MODULE(result), R_DESCRIPTION(result), channel->base.fd, channel->num_entries,
		channel->fence.id, channel->fence.value, channel->fence_incr,
		Deko3D::g_dk_command_buffer_mgr ? Deko3D::g_dk_command_buffer_mgr->GetCurrentFenceCounter() : 0,
		Deko3D::MemoryTracker::GetUsage() / (1024 * 1024));

	return result;
}

extern "C" Result __wrap_nvGpuChannelAppendEntry(NvGpuChannel* channel, iova_t address, size_t commands, u32 flags, u32 threshold)
{
	if (threshold < GPFIFO_QUEUE_SIZE && channel->num_entries >= GPFIFO_QUEUE_SIZE - threshold)
	{
		const Result result = __wrap_nvGpuChannelKickoff(channel);
		if (R_FAILED(result))
			return result;
	}
	return __real_nvGpuChannelAppendEntry(channel, address, commands, flags, threshold);
}
