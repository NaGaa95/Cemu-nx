// Switch 2 UamBridge: GLSL compiles with uam2rt (drako3d's UAM2: Mesa's GLSL front end and NAK), whose DKSH is
// complete as returned. Built into the uam library for the renderer and lsfg-deko3d alike.

#include "Cafe/HW/Latte/Renderer/Deko3D/UamBridge.h"

#include <cstdlib>
#include <cstring>

#include <uam2rt.h>

#if !defined(__SWITCH2__)
#error "UamBridgeSwitch2.cpp is only for the Switch 2 build"
#endif

static_assert(int(UamStage_Vertex) == int(Uam2rtStage_Vertex) && int(UamStage_TessControl) == int(Uam2rtStage_TessCtrl) &&
				  int(UamStage_TessEval) == int(Uam2rtStage_TessEval) && int(UamStage_Geometry) == int(Uam2rtStage_Geometry) &&
				  int(UamStage_Fragment) == int(Uam2rtStage_Fragment) && int(UamStage_Compute) == int(Uam2rtStage_Compute),
			  "uam2rt numbers the stages as uam");

void* UamCompileGlsl(const char* glsl, int stage, size_t* out_size, char** out_log)
{
	*out_size = 0;
	*out_log = nullptr;
	size_t size = 0;
	char* log = nullptr;
	Uam2rtInfo info{};
	void* image = uam2rtCompileGlsl(glsl, stage, &size, &log, &info);
	// (the callers free the log and the image with free(): uam2rt allocates them with malloc)
	*out_log = log;
	if (!image)
		return nullptr;
	*out_size = size;
	return image;
}
