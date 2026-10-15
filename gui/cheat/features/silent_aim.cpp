#include "silent_aim.hpp"
#include "../config.hpp"
#include "../driver/driver.hpp"
#include "../sdk/offsets.hpp"
#include "../sdk/vector.hpp"
#include <windows.h>
#include <cmath>

namespace Features {
    void SilentAim::Execute(const Matrix4x4& viewMatrix, int screenWidth, int screenHeight) {
        if (!g_Config.saim_enable || !(GetAsyncKeyState(g_Config.saim_key) & 0x8000)) return;
        if (Driver::hDriver == INVALID_HANDLE_VALUE || !Driver::baseAddress) return;

        uintptr_t world = Driver::ReadMemory<uintptr_t>(Driver::baseAddress + Offsets::World);
        if (!world) return;

        uintptr_t localPed = Driver::ReadMemory<uintptr_t>(world + Offsets::LocalPlayer);
        if (!localPed) return;

        uintptr_t replay = Driver::ReadMemory<uintptr_t>(Driver::baseAddress + Offsets::ReplayInterface);
        if (!replay) return;

        uintptr_t pedList = Driver::ReadMemory<uintptr_t>(replay + Offsets::ReplayPedList);
        int pedCount = Driver::ReadMemory<int>(replay + Offsets::ReplayPedCount);

        Vector2 screenCenter = { screenWidth / 2.0f, screenHeight / 2.0f };
        float closestDst = g_Config.saim_fov;
        Vector3 bestTargetHeadPos = { 0, 0, 0 };
        bool foundTarget = false;

        for (int i = 0; i < pedCount; ++i) {
            uintptr_t ped = Driver::ReadMemory<uintptr_t>(pedList + (i * Offsets::PedStride));
            if (!ped || ped == localPed) continue;

            float hp = Driver::ReadMemory<float>(ped + Offsets::CPed::Health);
            if (hp <= 100.0f) continue;

            uintptr_t nav = Driver::ReadMemory<uintptr_t>(ped + Offsets::CPed::Navigation);
            if (!nav) continue;

            Vector3 pos = Driver::ReadMemory<Vector3>(nav + Offsets::CNavigation::Position);
            Vector3 headPos = { pos.x, pos.y, pos.z + 0.8f };

            Vector2 headScreen;
            if (SDK::WorldToScreen(headPos, headScreen, viewMatrix, screenWidth, screenHeight)) {
                float dst = std::hypot(headScreen.x - screenCenter.x, headScreen.y - screenCenter.y);
                if (dst < closestDst) {
                    closestDst = dst;
                    bestTargetHeadPos = headPos;
                    foundTarget = true;
                }
            }
        }

        if (foundTarget) {
            // Write bullet target vector into WeaponManager
            uintptr_t weaponMgr = Driver::ReadMemory<uintptr_t>(localPed + Offsets::CPed::WeaponManager);
            if (weaponMgr) {
                // Offset 0x1A0 in CWeaponManager is the silent aim target position vector
                Driver::WriteMemory<Vector3>(weaponMgr + 0x1A0, bestTargetHeadPos);
            }
        }
    }
}
