#include "misc.hpp"
#include "../config.hpp"
#include "../driver/driver.hpp"
#include "../sdk/offsets.hpp"
#include "../sdk/vector.hpp"
#include <windows.h>

namespace Features {
    void Misc::Execute() {
        if (Driver::hDriver == INVALID_HANDLE_VALUE || !Driver::baseAddress) return;

        uintptr_t world = Driver::ReadMemory<uintptr_t>(Driver::baseAddress + Offsets::World);
        if (!world) return;

        uintptr_t localPed = Driver::ReadMemory<uintptr_t>(world + Offsets::LocalPlayer);
        if (!localPed) return;

        // Godmode & Refill HP/Armor
        if (g_Config.misc_godmode) {
            Driver::WriteMemory<float>(localPed + Offsets::CPed::Health, 200.0f);
            Driver::WriteMemory<float>(localPed + Offsets::CPed::Armor, 100.0f);
        }

        // Noclip (Fly)
        if (g_Config.misc_noclip && (GetAsyncKeyState(g_Config.misc_noclip_key) & 0x8000)) {
            uintptr_t nav = Driver::ReadMemory<uintptr_t>(localPed + Offsets::CPed::Navigation);
            if (nav) {
                Vector3 pos = Driver::ReadMemory<Vector3>(nav + Offsets::CNavigation::Position);
                pos.z += g_Config.misc_noclip_speed * 0.1f;
                Driver::WriteMemory<Vector3>(nav + Offsets::CNavigation::Position, pos);
            }
        }

        // Suicide
        if (g_Config.misc_suicide_key && (GetAsyncKeyState(g_Config.misc_suicide_key) & 0x8000)) {
            Driver::WriteMemory<float>(localPed + Offsets::CPed::Health, 0.0f);
        }
    }
}
