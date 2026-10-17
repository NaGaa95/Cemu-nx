#pragma once

#include "DKCommon.h"

#include <mutex>

namespace Deko3D
{
	class DKShaderCode
	{
	  public:
		~DKShaderCode();
		DKShaderCode(const DKShaderCode&) = delete;
		DKShaderCode& operator=(const DKShaderCode&) = delete;
		DkMemBlock GetMemory() const;
		u32 GetOffset() const { return m_offset; }
		void PrepareForUse(DkCmdBuf commands) const;

	  private:
		friend class DKShaderCodePool;
		struct Block;
		DKShaderCode() = default;
		std::shared_ptr<Block> m_block;
		u32 m_offset = 0;
		mutable bool m_needsInvalidate = true;
	};

	class DKShaderCodePool
	{
	  public:
		std::unique_ptr<DKShaderCode> Upload(std::span<const u8> code);

	  private:
		std::mutex m_mutex;
		std::vector<std::shared_ptr<DKShaderCode::Block>> m_blocks;
	};
} // namespace Deko3D
