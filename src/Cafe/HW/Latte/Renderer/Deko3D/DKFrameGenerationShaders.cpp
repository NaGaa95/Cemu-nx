// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DKFrameGenerator.h"
#include <lsfg/common/cache_format.hpp>
#include <lsfg/common/cache_store.hpp>
#include <lsfg/common/error.hpp>
#include <lsfg/common/prepare.hpp>

namespace Deko3D::FrameGeneration
{
	std::string GetCacheDirectory(bool performance)
	{
		return std::string("sdmc:/switch/cemu/lsfg/Cache/Deko3D/") + (performance ? "Performance" : "Quality");
	}

	bool PrepareShaders(bool performance, std::string& error)
	{
		const std::string directory = GetCacheDirectory(performance);
		lsfg::cache::Loaded cached;
		if (lsfg::succeeded(lsfg::cache::read(directory, cached)))
			return true;
		std::ifstream file("sdmc:/switch/cemu/lsfg/Lossless.dll", std::ios::binary | std::ios::ate);
		const auto size = file ? file.tellg() : std::streampos(-1);
		if (size <= 0 || size > 512 * 1024 * 1024)
		{
			error = "Lossless.dll could not be read";
			return false;
		}
		std::vector<u8> image(static_cast<size_t>(size));
		file.seekg(0);
		if (!file.read(reinterpret_cast<char*>(image.data()), image.size()))
		{
			error = "Lossless.dll could not be read";
			return false;
		}
		const lsfg::prepare::Options options{
			.precision = lsfg::shaders::Precision::high,
			.graph = {.performance = performance},
		};
		lsfg::prepare::Result result;
		const auto prepared = lsfg::prepare::run(image, options, result);
		if (!lsfg::succeeded(prepared))
		{
			error = fmt::format("LSFG shaders could not be prepared ({})", lsfg::error_name(prepared));
			return false;
		}
		std::error_code filesystemError;
		fs::create_directories(directory, filesystemError);
		const auto written = lsfg::cache::write(directory, result.contents);
		if (!lsfg::succeeded(written) || !lsfg::succeeded(lsfg::cache::read(directory, cached)) ||
			!lsfg::cache::same_modules(cached, result.contents))
		{
			error = "LSFG shader cache could not be saved and verified";
			return false;
		}
		return true;
	}
} // namespace Deko3D::FrameGeneration
