// The Switch 2 build's deko3d.hpp (cmake/switch/Deko3DDependencies.cmake puts this directory first): drako3d,
// deko3d's API on the Switch 2's Ampere GPU, with the same dk:: names, so the renderer builds unchanged against it.
#pragma once

#if !defined(__SWITCH2__)
#error "Renderer/Deko3D/Switch2 is only for the Switch 2 build"
#endif

#include <drako3d.hpp>
