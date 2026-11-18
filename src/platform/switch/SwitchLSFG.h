#pragma once

#include <vulkan/vulkan_core.h>

void SwitchLSFG_Configure(bool prepared, float flowScale, bool performanceMode);

bool SwitchLSFG_PrepareDeviceFeatures(VkPhysicalDevice physicalDevice, void*& featureChain,
	VkPhysicalDeviceTimelineSemaphoreFeatures& timelineFeature);
void SwitchLSFG_SetDevice(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
	VkQueue queue, uint32_t queueFamily, PFN_vkGetInstanceProcAddr getInstanceProcAddr);
void SwitchLSFG_ResetDevice();

bool SwitchLSFG_AdjustSwapchainCreateInfo(bool mainWindow, VkSwapchainCreateInfoKHR& createInfo,
	const VkSurfaceCapabilitiesKHR& capabilities);
void SwitchLSFG_AttachSwapchain(bool mainWindow, VkSwapchainKHR swapchain, VkExtent2D extent,
	const VkImage* images, uint32_t imageCount, bool compatible);
void SwitchLSFG_DetachSwapchain(VkSwapchainKHR swapchain);

bool SwitchLSFG_Present(VkQueue queue, const VkPresentInfoKHR& presentInfo, VkResult& result);

bool SwitchLSFG_IsPrepared();
bool SwitchLSFG_IsAvailable();
bool SwitchLSFG_IsEnabled();
bool SwitchLSFG_IsHighRatePassthrough();
// A 50/60 FPS game while frame generation is on (kept until it is off): the Switch 2's docked 120 Hz trigger.
bool SwitchLSFG_IsHighRateSource();
void SwitchLSFG_RequestEnabled(bool enabled);
