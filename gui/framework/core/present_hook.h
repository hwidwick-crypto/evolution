#pragma once

#include <windows.h>

// In-game rendering engine. When evolution_gui.dll is loaded into GTA5.exe it
// installs a DXGI Present / ResizeBuffers vtable hook and renders the ImGui
// menu + ESP directly over the game frame (INSERT toggles the menu).
namespace present_hook
{
    // Spawn the bootstrap thread (vtable patch + ImGui init + feature loop).
    bool start(HMODULE self_module);
}
