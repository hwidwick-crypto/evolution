//
// Evolution GUI - framework main.cpp (DLL build)
//
// This file is compiled into evolution_gui.dll.
// The actual DLL entry point is in dllmain.cpp.
// This file is intentionally empty - all GUI code is in gui.cpp and the
// render loop runs inside dllmain.cpp's GuiThread().
//

// NOTE: do NOT add `#pragma comment(linker, "/ENTRY:DllMain")` here.
// Overriding the entry point bypasses the CRT glue (dll_dllmain.obj), which is
// what carries /DEFAULTLIB:vcruntime.lib and /DEFAULTLIB:ucrt.lib. Without it
// the link fails with dozens of unresolved CRT symbols, and even a successful
// link would skip CRT initialization. The default entry point
// (DllMainCRTStartup) calls DllMain() from dllmain.cpp after initializing the
// CRT, which is exactly what this DLL needs.