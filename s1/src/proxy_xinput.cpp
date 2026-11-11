// "Door" number 3: pretends to be XInput1_3.dll, hands every real request to the genuine
// Windows XInput1_3.dll, and starts the theater mod (S1iris) as a side effect.
//
// Why this door exists: on some PCs s1x.exe loads the real version.dll early (a graphics-driver DLL
// pulls it in), so a version.dll in the game folder is never even looked at. The game asks for
// XInput1_3.dll from the game folder a few milliseconds after it is loaded, and nothing loads it earlier.

#include "theater.hpp"

// The game imports this DLL by ORDINAL number ("import #2"), so every export must sit at the same ordinal
// as in the real XInput1_3.dll, and each one is forwarded by that same ordinal (so it always reaches the
// matching real function, whatever it is called). Real layout: 1 DllMain, 2..8 the documented functions,
// 100..103 the unnamed ones (extended state, guide-button wait/cancel, power off).
#define FWD_NAMED(name, n) \
	__pragma(comment(linker, "/export:" #name "=\"C:\\Windows\\System32\\XInput1_3.#" #n "\",@" #n))
#define FWD_NONAME(label, n) \
	__pragma(comment(linker, "/export:" #label "=\"C:\\Windows\\System32\\XInput1_3.#" #n "\",@" #n ",NONAME"))

FWD_NONAME(XInputOrd1, 1)
FWD_NAMED(XInputGetState, 2)
FWD_NAMED(XInputSetState, 3)
FWD_NAMED(XInputGetCapabilities, 4)
FWD_NAMED(XInputEnable, 5)
FWD_NAMED(XInputGetDSoundAudioDeviceGuids, 6)
FWD_NAMED(XInputGetBatteryInformation, 7)
FWD_NAMED(XInputGetKeystroke, 8)
FWD_NONAME(XInputOrd100, 100)
FWD_NONAME(XInputOrd101, 101)
FWD_NONAME(XInputOrd102, 102)
FWD_NONAME(XInputOrd103, 103)

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(module);
		theater_start(module);
	}

	return TRUE;
}
