//
// Evolution GUI - in-game render hook
//
// Installed when evolution_gui.dll is loaded into the game process (the loader
// injects it). We hook the shared IDXGISwapChain vtable (Present / ResizeBuffers)
// and draw the ImGui menu + ESP directly over the game frame - no separate
// window, no background of our own.
//
// Controls: INSERT (menu_key) toggles the menu, ESC closes it, END (unload_key)
// unhooks everything, cleans traces and frees the library.
//

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <thread>

#include "present_hook.h"

#include "gui.h"
#include "glow_renderer.h"
#include "screenshot.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "config.hpp"
#include "driver/driver.hpp"
#include "utils/cleaner.hpp"
#include "sdk/offsets.hpp"
#include "sdk/matrix.hpp"
#include "features/aimbot.hpp"
#include "features/silent_aim.hpp"
#include "features/triggerbot.hpp"
#include "features/esp.hpp"
#include "features/misc.hpp"

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

// Dear ImGui keeps this declaration commented out in imgui_impl_win32.h
// (inside #if 0), so it must be forward-declared here.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
    using PresentFn        = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    using ResizeBuffersFn  = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

    // IDXGISwapChain vtable slots (IUnknown 0-2, DeviceSubObject 3, Object 4-7).
    constexpr int kPresentSlot       = 8;
    constexpr int kResizeBuffersSlot = 13;

    PresentFn       g_originalPresent       = nullptr;
    ResizeBuffersFn g_originalResizeBuffers = nullptr;

    std::atomic<bool> g_running{ false };       // hook is live
    std::atomic<bool> g_imguiReady{ false };
    std::atomic<bool> g_menuVisible{ false };
    std::atomic<bool> g_unloadRequested{ false };
    std::atomic<bool> g_stop{ false };

    std::atomic<HWND> g_gameHwnd{ nullptr };
    std::atomic<UINT> g_width{ 0 };
    std::atomic<UINT> g_height{ 0 };

    HMODULE g_self = nullptr;

    IDXGISwapChain*    g_swapChain     = nullptr;
    ID3D11Device*      g_device        = nullptr;
    ID3D11DeviceContext* g_context     = nullptr;
    ID3D11RenderTargetView* g_rtv      = nullptr;
    WNDPROC            g_originalWndProc = nullptr;

    std::thread g_bootThread;
    std::thread g_featureThread;
    std::atomic<bool> g_bootStarted{ false };

    // -----------------------------------------------------------------------
    // Render target helpers
    // -----------------------------------------------------------------------

    void destroy_rtv()
    {
        if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    }

    bool create_rtv()
    {
        destroy_rtv();
        if (!g_swapChain || !g_device)
            return false;

        ID3D11Texture2D* backBuffer = nullptr;
        if (FAILED(g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) || !backBuffer)
            return false;

        const HRESULT hr = g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv);
        backBuffer->Release();
        return SUCCEEDED(hr);
    }

    // -----------------------------------------------------------------------
    // Game state helpers
    // -----------------------------------------------------------------------

    Matrix4x4 read_view_matrix()
    {
        Matrix4x4 matrix{};
        if (Driver::hDriver == INVALID_HANDLE_VALUE || !Driver::baseAddress)
            return matrix;

        const uintptr_t viewport = Driver::ReadMemory<uintptr_t>(Driver::baseAddress + Offsets::Viewport);
        if (viewport)
            matrix = Driver::ReadMemory<Matrix4x4>(viewport + Offsets::ViewMatrix);

        return matrix;
    }

    void apply_obs_bypass()
    {
        static int applied = -1;
        const int want = g_Config.obs_bypass ? 1 : 0;
        if (want == applied)
            return;

        if (HWND hwnd = g_gameHwnd.load())
            SetWindowDisplayAffinity(hwnd, want ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);

        applied = want;
    }

    void poll_hotkeys()
    {
        static bool menuWasDown = false;
        static bool unloadWasDown = false;

        const int menuKey = g_Config.menu_key ? g_Config.menu_key : 0x2D;
        const bool menuDown = (GetAsyncKeyState(menuKey) & 0x8000) != 0;
        if (menuDown && !menuWasDown)
            g_menuVisible = !g_menuVisible;
        menuWasDown = menuDown;

        if (g_menuVisible && (GetAsyncKeyState(VK_ESCAPE) & 0x8000))
            g_menuVisible = false;

        if (g_Config.unload_key)
        {
            const bool unloadDown = (GetAsyncKeyState(g_Config.unload_key) & 0x8000) != 0;
            if (unloadDown && !unloadWasDown)
                g_unloadRequested = true;
            unloadWasDown = unloadDown;
        }

        apply_obs_bypass();
    }

    // -----------------------------------------------------------------------
    // Feature loop (runs on its own thread, ~200 Hz)
    // -----------------------------------------------------------------------

    void feature_thread()
    {
        // Wait for the kernel driver (mapped by the loader before injection).
        while (!g_stop && !Driver::InitDriver())
            Sleep(500);

        if (g_stop)
            return;

        Driver::targetPid = GetCurrentProcessId();

        while (!g_stop)
        {
            if (!Driver::baseAddress)
            {
                Driver::baseAddress = Driver::GetModuleBaseAddress(Driver::targetPid);
                if (!Driver::baseAddress)
                {
                    Sleep(500);
                    continue;
                }
            }

            const Matrix4x4 viewMatrix = read_view_matrix();

            HWND hwnd = g_gameHwnd.load();
            int width  = (int)g_width.load();
            int height = (int)g_height.load();
            if (hwnd)
            {
                RECT rc{};
                if (GetClientRect(hwnd, &rc))
                {
                    width  = rc.right - rc.left;
                    height = rc.bottom - rc.top;
                }
            }

            Features::Aimbot::Execute(viewMatrix, width, height);
            Features::SilentAim::Execute(viewMatrix, width, height);
            Features::Triggerbot::Execute(viewMatrix, width, height);
            Features::Misc::Execute();

            Sleep(5);
        }

        Driver::CloseDriver();
    }

    // -----------------------------------------------------------------------
    // Game window subclass - feed ImGui, swallow game input while menu open
    // -----------------------------------------------------------------------

    bool is_input_message(UINT msg)
    {
        switch (msg)
        {
        case WM_KEYDOWN: case WM_KEYUP:
        case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        case WM_CHAR: case WM_SYSCHAR: case WM_UNICHAR:
        case WM_MOUSEMOVE: case WM_MOUSELEAVE:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_INPUT:
            return true;
        default:
            return false;
        }
    }

    LRESULT CALLBACK hooked_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (g_imguiReady && g_menuVisible)
        {
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);

            // Keep the input away from the game while the menu is open.
            if (is_input_message(msg))
                return 0;
        }

        if (g_originalWndProc)
            return CallWindowProcW(g_originalWndProc, hwnd, msg, wParam, lParam);

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    // -----------------------------------------------------------------------
    // ImGui lifecycle
    // -----------------------------------------------------------------------

    bool init_imgui(IDXGISwapChain* swapChain)
    {
        if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device))) || !g_device)
            return false;

        g_device->GetImmediateContext(&g_context);

        DXGI_SWAP_CHAIN_DESC desc{};
        swapChain->GetDesc(&desc);

        g_swapChain = swapChain;
        g_gameHwnd = desc.OutputWindow;
        g_width  = desc.BufferDesc.Width;
        g_height = desc.BufferDesc.Height;

        if (!create_rtv())
            return false;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;

        ImGui_ImplWin32_Init(desc.OutputWindow);
        ImGui_ImplDX11_Init(g_device, g_context);
        gui::initialize(g_device);

        if (desc.OutputWindow)
            g_originalWndProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(desc.OutputWindow, GWLP_WNDPROC,
                    reinterpret_cast<LONG_PTR>(hooked_wnd_proc)));

        g_imguiReady = true;
        return true;
    }

    void shutdown_imgui()
    {
        const bool wasReady = g_imguiReady.exchange(false);

        // Give an in-flight hooked frame time to finish before tearing down.
        if (wasReady)
            Sleep(150);

        if (HWND hwnd = g_gameHwnd.load())
        {
            if (g_originalWndProc)
                SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));
            g_originalWndProc = nullptr;
        }

        // Only tear ImGui down if it was fully initialized (an early init
        // failure leaves the context unreferenced).
        if (wasReady)
        {
            gui::shutdown();
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
        }

        destroy_rtv();
        if (g_context) { g_context->Release(); g_context = nullptr; }
        if (g_device)  { g_device->Release();  g_device  = nullptr; }
        g_swapChain = nullptr;
        g_gameHwnd = nullptr;
    }

    // -----------------------------------------------------------------------
    // Frame
    // -----------------------------------------------------------------------

    void render_frame()
    {
        poll_hotkeys();

        screenshot::begin_frame();

        gui::prepare_frame();
        ImGui::NewFrame();

        ImGuiIO& io = ImGui::GetIO();
        io.MouseDrawCursor = g_menuVisible.load();

        if (g_Config.esp_master)
        {
            const Matrix4x4 viewMatrix = read_view_matrix();
            Features::ESP::Render(viewMatrix, (int)g_width.load(), (int)g_height.load());
        }

        if (g_menuVisible)
            gui::render(g_gameHwnd.load());

        ImGui::Render();

        if (g_rtv)
        {
            g_context->OMSetRenderTargets(1, &g_rtv, nullptr);

            glow_renderer::flush(g_context, ImGui::GetIO().DisplaySize);
            glow_renderer::clear_queue();

            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }

        screenshot::state& ss = screenshot::get();
        if (ss.capture_frame && ss.bounds_valid && g_rtv)
        {
            screenshot::capture_and_save(g_device, g_context, g_rtv, ss.bounds_min, ss.bounds_max);
            ss.capture_frame = false;
            ss.bounds_valid  = false;
        }
    }

    // -----------------------------------------------------------------------
    // DXGI hooks
    // -----------------------------------------------------------------------

    HRESULT STDMETHODCALLTYPE hooked_present(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
    {
        if (g_running && swapChain)
        {
            if (!g_imguiReady)
            {
                // The first swapchain that presents after the patch is the
                // game's main one. Try it once - never re-init on the same
                // object (a failed attempt would leak the device reference).
                static IDXGISwapChain* attempted = nullptr;
                if (swapChain != attempted)
                {
                    attempted = swapChain;
                    init_imgui(swapChain);
                }
            }

            if (g_imguiReady && swapChain == g_swapChain && !g_unloadRequested)
                render_frame();
        }

        return g_originalPresent(swapChain, syncInterval, flags);
    }

    HRESULT STDMETHODCALLTYPE hooked_resize_buffers(IDXGISwapChain* swapChain, UINT bufferCount,
        UINT width, UINT height, DXGI_FORMAT format, UINT flags)
    {
        if (g_imguiReady && swapChain == g_swapChain)
            destroy_rtv();

        const HRESULT hr = g_originalResizeBuffers(swapChain, bufferCount, width, height, format, flags);

        if (g_imguiReady && swapChain == g_swapChain)
        {
            g_width  = width;
            g_height = height;
            create_rtv();
        }

        return hr;
    }

    void patch_slot(void** vtable, int slot, void* replacement, void** original)
    {
        DWORD oldProtect = 0;
        if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
            return;

        *original = InterlockedExchangePointer(&vtable[slot], replacement);

        DWORD temp = 0;
        VirtualProtect(&vtable[slot], sizeof(void*), oldProtect, &temp);
    }

    void restore_slot(void** vtable, int slot, void* original)
    {
        DWORD oldProtect = 0;
        if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
            return;

        InterlockedExchangePointer(&vtable[slot], original);

        DWORD temp = 0;
        VirtualProtect(&vtable[slot], sizeof(void*), oldProtect, &temp);
    }

    // The vtable lives in the shared DXGI module, so patching the slots of a
    // dummy swapchain affects every swapchain in the process - including the
    // game's, even if it was created before we injected.
    void** g_patchedVtable = nullptr;

    bool patch_dxgi_vtable()
    {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc   = DefWindowProcW;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"EvolutionDummyWnd";

        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;

        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
            0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd)
            return false;

        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount       = 1;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow      = hwnd;
        sd.SampleDesc.Count  = 1;
        sd.Windowed          = TRUE;
        sd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;

        ID3D11Device*        device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        IDXGISwapChain*      chain  = nullptr;
        D3D_FEATURE_LEVEL    level  = {};

        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            0, nullptr, 0, D3D11_SDK_VERSION, &sd, &chain, &device, &level, &context);

        if (FAILED(hr))
        {
            hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                0, nullptr, 0, D3D11_SDK_VERSION, &sd, &chain, &device, &level, &context);
        }

        if (FAILED(hr) || !chain)
        {
            DestroyWindow(hwnd);
            UnregisterClassW(wc.lpszClassName, wc.hInstance);
            return false;
        }

        void** vtable = *reinterpret_cast<void***>(chain);
        g_patchedVtable = vtable;
        patch_slot(vtable, kPresentSlot, reinterpret_cast<void*>(&hooked_present),
            reinterpret_cast<void**>(&g_originalPresent));
        patch_slot(vtable, kResizeBuffersSlot, reinterpret_cast<void*>(&hooked_resize_buffers),
            reinterpret_cast<void**>(&g_originalResizeBuffers));

        if (context) context->Release();
        if (device)  device->Release();
        chain->Release();

        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);

        return g_originalPresent && g_originalResizeBuffers;
    }

    void unpatch_dxgi_vtable()
    {
        if (!g_patchedVtable)
            return;

        if (g_originalPresent)
            restore_slot(g_patchedVtable, kPresentSlot, reinterpret_cast<void*>(g_originalPresent));
        if (g_originalResizeBuffers)
            restore_slot(g_patchedVtable, kResizeBuffersSlot, reinterpret_cast<void*>(g_originalResizeBuffers));

        g_patchedVtable = nullptr;
    }

    // -----------------------------------------------------------------------
    // Bootstrap / teardown
    // -----------------------------------------------------------------------

    void bootstrap_thread()
    {
        // Let the loader lock release before touching anything.
        Sleep(300);

        if (!patch_dxgi_vtable())
            return;

        g_running = true;
        g_featureThread = std::thread(feature_thread);

        while (!g_unloadRequested)
            Sleep(50);

        // Stop using the hook, then restore it.
        g_running = false;
        Sleep(200);
        unpatch_dxgi_vtable();

        g_stop = true;
        if (g_featureThread.joinable())
            g_featureThread.join();

        shutdown_imgui();

        Utils::Cleaner::SanitizeRegistryTraces();

        // Unload the library from a fresh thread (never from the game's
        // render thread).
        std::thread([] {
            Sleep(100);
            if (g_self)
                FreeLibraryAndExitThread(g_self, 0);
        }).detach();
    }
}

namespace present_hook
{
    bool start(HMODULE self_module)
    {
        if (g_bootStarted.exchange(true))
            return true;

        g_self = self_module;
        g_bootThread = std::thread(bootstrap_thread);
        g_bootThread.detach();
        return true;
    }
}
