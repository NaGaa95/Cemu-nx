#include "DKShaderThread.h"

#include <system_error>
#include <utility>

namespace Deko3D
{
	ShaderThread::ShaderThread(std::function<void()> entry) : m_entry(std::move(entry))
	{
		pthread_attr_t attributes;
		int error = pthread_attr_init(&attributes);
		if (error)
			throw std::system_error(error, std::generic_category(), "Shader thread attributes");
		error = pthread_attr_setstacksize(&attributes, 2 * 1024 * 1024);
		if (!error)
			error = pthread_create(&m_thread, &attributes, [](void* context) -> void* {
				static_cast<ShaderThread*>(context)->m_entry();
				return nullptr; }, this);
		pthread_attr_destroy(&attributes);
		if (error)
			throw std::system_error(error, std::generic_category(), "Shader thread creation");
	}

	ShaderThread::~ShaderThread()
	{
		pthread_join(m_thread, nullptr);
	}
} // namespace Deko3D
