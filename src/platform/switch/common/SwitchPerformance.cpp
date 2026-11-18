#include <switch.h>

// (a Switch 1 profile: libounce ends the program when the Switch 2 refuses it)
#if !defined(__SWITCH2__)
extern "C"
{
u32 __nx_applet_PerformanceConfiguration[2] = {0x92220007, 0};
}
#endif
