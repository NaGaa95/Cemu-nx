#pragma once

#include "DKImage.h"

namespace Deko3D
{
	struct FrameGenerationConfig
	{
		bool performance = true;
		u32 multiplier = 2;
		u32 flow_scale = 4;
	};

	struct FrameRect
	{
		int left, top, width, height;
		int GetWidth() const
		{
			return width;
		}
		int GetHeight() const
		{
			return height;
		}
	};

	class FrameGenerator
	{
	  public:
		virtual ~FrameGenerator() = default;
		virtual bool Generate(const DKImage* frame, const FrameRect& rect) = 0;
		virtual const DKImage* GetGeneratedFrame(u32 index) const = 0;
		virtual u32 GetGeneratedFrameCount() const = 0;
		virtual std::chrono::nanoseconds GetDisplayInterval() const = 0;
		virtual void Reset() = 0;
	};

	std::unique_ptr<FrameGenerator> CreateFrameGenerator(const FrameGenerationConfig& config);
	void InvalidateCachedState();

	namespace FrameGeneration
	{
		std::string GetCacheDirectory(bool performance);
		bool PrepareShaders(bool performance, std::string& error);
	} // namespace FrameGeneration
} // namespace Deko3D
