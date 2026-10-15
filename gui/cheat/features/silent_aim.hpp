#pragma once
#include "../sdk/gta5.hpp"

namespace Features {
    class SilentAim {
    public:
        static void Execute(const Matrix4x4& viewMatrix, int screenWidth, int screenHeight);
    };
}
