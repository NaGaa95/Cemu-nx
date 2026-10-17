#pragma once

#include <functional>
#include <pthread.h>

namespace Deko3D
{
	class ShaderThread
	{
	  public:
		explicit ShaderThread(std::function<void()> entry);
		~ShaderThread();
		ShaderThread(const ShaderThread&) = delete;
		ShaderThread& operator=(const ShaderThread&) = delete;

	  private:
		std::function<void()> m_entry;
		pthread_t m_thread{};
	};
} // namespace Deko3D
