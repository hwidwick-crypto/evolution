#pragma once
#include <cstdint>

namespace Offsets {

    constexpr uintptr_t World = 0x25667E8;
    constexpr uintptr_t ReplayInterface = 0x1F03958;
    constexpr uintptr_t Viewport = 0x1FE93E0;
    constexpr uintptr_t LocalPlayer = 0x8;

    // Viewport -> view matrix (Matrix4x4) lives behind this pointer.
    constexpr uintptr_t ViewMatrix = 0x24C;

    // ReplayInterface internals.
    constexpr uintptr_t ReplayPedCount = 0x10;
    constexpr uintptr_t ReplayPedList  = 0x18;
    constexpr uintptr_t PedStride      = 0x10;

    namespace CPed {
        constexpr uintptr_t Navigation = 0x30;
        constexpr uintptr_t Health = 0x280;
        constexpr uintptr_t MaxHealth = 0x2A0;
        constexpr uintptr_t Armor = 0x14E0;
        constexpr uintptr_t WeaponManager = 0x10D8;
        constexpr uintptr_t Vehicle = 0xD30;
        constexpr uintptr_t IsInVehicle = 0x146B;
        constexpr uintptr_t BoneList = 0x430;
    }


    namespace CNavigation {
        constexpr uintptr_t Position = 0x50;
        constexpr uintptr_t Rotation = 0x30;
    }


    namespace Bones {
        constexpr int SKEL_HEAD = 0x796E;
        constexpr int SKEL_NECK = 0x9995;
        constexpr int SKEL_SPINE3 = 0xE0E6;
        constexpr int SKEL_PELVIS = 0x2E28;
        constexpr int SKEL_L_FOOT = 0x3779;
        constexpr int SKEL_R_FOOT = 0xCC72;
    }
}
