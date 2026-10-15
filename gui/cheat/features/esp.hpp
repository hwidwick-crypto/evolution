#pragma once
#include "../sdk/gta5.hpp"

namespace Features {
    class ESP {
    public:
        // Must be called inside an active ImGui frame - draws into the
        // background draw list (in-game overlay).
        static void Render(const Matrix4x4& viewMatrix, int screenWidth, int screenHeight);
    };
}
