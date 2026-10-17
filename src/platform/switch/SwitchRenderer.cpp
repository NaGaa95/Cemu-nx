#include "Cafe/HW/Latte/Renderer/Renderer.h"
#if defined(ENABLE_VULKAN)
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#endif
#if defined(ENABLE_DEKO3D)
#include "Cafe/HW/Latte/Renderer/Deko3D/Deko3DRenderer.h"
#endif
#include "config/CemuConfig.h"
#include "platform/switch/SwitchPlatform.h"
#include "platform/switch/SwitchSwkbd.h"

#include <cstdlib>
#include <memory>
#include <string_view>

#if defined(ENABLE_OPENGL)
#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.h"

#include <EGL/egl.h>
extern "C"
{
#include <switch/display/native_window.h>
}
#endif

namespace
{
#if defined(ENABLE_OPENGL)
class SwitchOpenGLCanvas final : public OpenGLCanvasCallbacks
{
public:
	~SwitchOpenGLCanvas() override
	{
		Shutdown();
	}

	bool Initialize()
	{
		m_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
		if (m_display == EGL_NO_DISPLAY || eglInitialize(m_display, nullptr, nullptr) != EGL_TRUE)
			return false;
		if (eglBindAPI(EGL_OPENGL_API) != EGL_TRUE)
			return false;

		const EGLint configAttributes[] = {
			EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
			EGL_RED_SIZE, 8,
			EGL_GREEN_SIZE, 8,
			EGL_BLUE_SIZE, 8,
			EGL_ALPHA_SIZE, 8,
			EGL_DEPTH_SIZE, 24,
			EGL_STENCIL_SIZE, 8,
			EGL_NONE,
		};
		EGLConfig config = nullptr;
		EGLint configCount = 0;
		if (eglChooseConfig(m_display, configAttributes, &config, 1, &configCount) != EGL_TRUE ||
			configCount < 1)
		{
			return false;
		}

		m_surface = eglCreateWindowSurface(m_display, config,
			static_cast<EGLNativeWindowType>(nwindowGetDefault()), nullptr);
		if (m_surface == EGL_NO_SURFACE)
			return false;

		const EGLint compatibility45[] = {
			EGL_CONTEXT_MAJOR_VERSION, 4,
			EGL_CONTEXT_MINOR_VERSION, 5,
			EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
			EGL_NONE,
		};
		const EGLint compatibility43[] = {
			EGL_CONTEXT_MAJOR_VERSION, 4,
			EGL_CONTEXT_MINOR_VERSION, 3,
			EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
			EGL_NONE,
		};
		m_context = eglCreateContext(m_display, config, EGL_NO_CONTEXT, compatibility45);
		if (m_context == EGL_NO_CONTEXT)
			m_context = eglCreateContext(m_display, config, EGL_NO_CONTEXT, compatibility43);
		if (m_context == EGL_NO_CONTEXT)
			m_context = eglCreateContext(m_display, config, EGL_NO_CONTEXT, nullptr);
		if (m_context == EGL_NO_CONTEXT ||
			eglMakeCurrent(m_display, m_surface, m_surface, m_context) != EGL_TRUE)
		{
			return false;
		}

		// The Latte thread owns the context during emulation. Release it from
		// the launcher thread before that thread starts.
		if (eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE)
			return false;
		SetOpenGLCanvasCallbacks(this);
		m_callbacksRegistered = true;
		return true;
	}

	bool HasPadViewOpen() const override
	{
		return SwitchPlatform_IsGamePadOutputActive();
	}

	bool MakeCurrent(bool) override
	{
		if (m_display == EGL_NO_DISPLAY || m_surface == EGL_NO_SURFACE ||
			m_context == EGL_NO_CONTEXT)
		{
			return false;
		}
		if (eglGetCurrentContext() != m_context &&
			eglMakeCurrent(m_display, m_surface, m_surface, m_context) != EGL_TRUE)
		{
			return false;
		}
		const int interval = GetConfig().vsync.GetValue() > 0 ? 1 : 0;
		if (interval != m_swapInterval && eglSwapInterval(m_display, interval) == EGL_TRUE)
			m_swapInterval = interval;
		return true;
	}

	void SwapBuffers(bool swapTV, bool swapDRC) override
	{
		const bool shouldPresent = swapTV ||
			(SwitchPlatform_IsGamePadOutputActive() && swapDRC);
		if (!shouldPresent || SwitchSwkbd_IsAppletActive())
			return;

		if (!MakeCurrent(false))
			return;

		if (eglSwapBuffers(m_display, m_surface) == EGL_TRUE)
			SwitchPlatform_NotifyGameFrameSubmitted();
	}

	void ReleaseCurrent()
	{
		if (m_display != EGL_NO_DISPLAY && eglGetCurrentContext() == m_context)
			eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	}

	void Shutdown()
	{
		if (m_callbacksRegistered)
		{
			ClearOpenGLCanvasCallbacks();
			m_callbacksRegistered = false;
		}
		ReleaseCurrent();
		if (m_display != EGL_NO_DISPLAY && m_context != EGL_NO_CONTEXT)
		{
			eglDestroyContext(m_display, m_context);
			m_context = EGL_NO_CONTEXT;
		}
		if (m_display != EGL_NO_DISPLAY && m_surface != EGL_NO_SURFACE)
		{
			eglDestroySurface(m_display, m_surface);
			m_surface = EGL_NO_SURFACE;
		}
		if (m_display != EGL_NO_DISPLAY)
		{
			eglTerminate(m_display);
			m_display = EGL_NO_DISPLAY;
		}
	}

private:
	EGLDisplay m_display = EGL_NO_DISPLAY;
	EGLSurface m_surface = EGL_NO_SURFACE;
	EGLContext m_context = EGL_NO_CONTEXT;
	int m_swapInterval = -1;
	bool m_callbacksRegistered = false;
};

std::unique_ptr<SwitchOpenGLCanvas> s_openGLCanvas;
#endif

void ConfigureMesaBackend(std::string_view backend)
{
	if (backend == "gl" || backend == "zink")
	{
		const bool zink = backend == "zink";
		// Cemu already owns a dedicated Latte render thread. A second Mesa GL
		// command thread only adds latency and complicates context shutdown.
		setenv("MESA_SWITCH_GLTHREAD", "0", 1);
		setenv("MESA_SWITCH_GL_DRIVER", zink ? "zink" : "nvc0", 1);
		setenv("MESA_LOADER_DRIVER_OVERRIDE", zink ? "zink" : "nouveau", 1);
		unsetenv("NOUVEAU_SWITCH_GM20B_MME");
		unsetenv("NOUVEAU_SWITCH_FAST_DRAW");
	}
	else
	{
		unsetenv("MESA_SWITCH_GLTHREAD");
		unsetenv("MESA_SWITCH_GL_DRIVER");
		unsetenv("MESA_LOADER_DRIVER_OVERRIDE");
		unsetenv("NOUVEAU_SWITCH_GM20B_MME");
		unsetenv("NOUVEAU_SWITCH_FAST_DRAW");
	}
}
}

bool SwitchCreateRenderer(int width, int height, std::string_view backend)
{
	ConfigureMesaBackend(backend);
	try
	{
#if defined(ENABLE_DEKO3D)
		if (backend == "deko3d")
		{
			g_renderer = std::make_unique<Deko3DRenderer>();
			return true;
		}
#endif
		if (backend == "gl" || backend == "zink")
		{
#if defined(ENABLE_OPENGL)
			s_openGLCanvas = std::make_unique<SwitchOpenGLCanvas>();
			if (!s_openGLCanvas->Initialize())
			{
				s_openGLCanvas.reset();
				return false;
			}
			g_renderer = std::make_unique<OpenGLRenderer>();
			return true;
#else
			return false;
#endif
		}

#if defined(ENABLE_VULKAN)
		if (!InitializeGlobalVulkan())
			return false;
		g_renderer = std::make_unique<VulkanRenderer>();
		VulkanRenderer::GetInstance()->InitializeSurface({width, height}, true);
		return true;
#else
		return false;
#endif
	}
	catch (...)
	{
		g_renderer.reset();
#if defined(ENABLE_OPENGL)
		s_openGLCanvas.reset();
#endif
		return false;
	}
}

void SwitchRendererThreadExit()
{
#if defined(ENABLE_OPENGL)
	if (s_openGLCanvas)
		s_openGLCanvas->ReleaseCurrent();
#endif
}

void SwitchDestroyRenderer()
{
#if defined(ENABLE_OPENGL)
	s_openGLCanvas.reset();
#endif
}
