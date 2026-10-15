#include "aimbot.hpp"
#include "../config.hpp"
#include "../driver/driver.hpp"
#include "../sdk/offsets.hpp"
#include <windows.h>
#include <cmath>

namespace Features {
    void Aimbot::Execute(const Matrix4x4& viewMatrix, int screenWidth, int screenHeight) {
        if (!g_Config.vaim_enable || !(GetAsyncKeyState(g_Config.vaim_key) & 0x8000)) return;
        if (Driver::hDriver == INVALID_HANDLE_VALUE || !Driver::baseAddress) return;

        uintptr_t world = Driver::ReadMemory<uintptr_t>(Driver::baseAddress + Offsets::World);
        if (!world) return;

        uintptr_t localPed = Driver::ReadMemory<uintptr_t>(world + Offsets::LocalPlayer);
        if (!localPed) return;

        uintptr_t replay = Driver::ReadMemory<uintptr_t>(Driver::baseAddress + Offsets::ReplayInterface);
        if (!replay) return;

        uintptr_t pedList = Driver::ReadMemory<uintptr_t>(replay + Offsets::ReplayPedList);
        int pedCount = Driver::ReadMemory<int>(replay + Offsets::ReplayPedCount);

        Vector3 localPos{};
        if (uintptr_t localNav = Driver::ReadMemory<uintptr_t>(localPed + Offsets::CPed::Navigation))
            localPos = Driver::ReadMemory<Vector3>(localNav + Offsets::CNavigation::Position);

        Vector2 screenCenter = { screenWidth / 2.0f, screenHeight / 2.0f };
        float closestDst = g_Config.vaim_fov;
        Vector2 targetScreen = { 0, 0 };
        bool foundTarget = false;

        for (int i = 0; i < pedCount; ++i) {
            uintptr_t ped = Driver::ReadMemory<uintptr_t>(pedList + (i * Offsets::PedStride));
            if (!ped || ped == localPed) continue;

            float hp = Driver::ReadMemory<float>(ped + Offsets::CPed::Health);
            if (hp <= 100.0f && g_Config.vaim_ignore_dead) continue;

            uintptr_t nav = Driver::ReadMemory<uintptr_t>(ped + Offsets::CPed::Navigation);
            if (!nav) continue;

            Vector3 pos = Driver::ReadMemory<Vector3>(nav + Offsets::CNavigation::Position);

            // Cull targets beyond the configured max distance.
            if (g_Config.vaim_max_dist > 0.0f && localPos.Distance(pos) > g_Config.vaim_max_dist)
                continue;

            Vector3 headPos = { pos.x, pos.y, pos.z + 0.8f };

            Vector2 headScreen;
            if (SDK::WorldToScreen(headPos, headScreen, viewMatrix, screenWidth, screenHeight)) {
                float dst = std::hypot(headScreen.x - screenCenter.x, headScreen.y - screenCenter.y);
                if (dst < closestDst) {
                    closestDst = dst;
                    targetScreen = headScreen;
                    foundTarget = true;
                }
            }
        }

        if (foundTarget) {
            const float smooth = g_Config.vaim_smooth < 1.0f ? 1.0f : g_Config.vaim_smooth;
            float deltaX = (targetScreen.x - screenCenter.x) / smooth;
            float deltaY = (targetScreen.y - screenCenter.y) / smooth;
            mouse_event(MOUSEEVENTF_MOVE, static_cast<DWORD>(deltaX), static_cast<DWORD>(deltaY), 0, 0);
        }
    }
}
