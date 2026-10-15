#pragma once

#include <windows.h>

struct ID3D11Device;

namespace gui
{
    void initialize(ID3D11Device* device);
    void shutdown();
    void prepare_frame();
    void render(HWND hwnd);

    // Menu state -> cheat config (g_Config) sync, called at the end of render().
    void sync_cheat_config();
}
