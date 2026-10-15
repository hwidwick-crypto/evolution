#pragma once
#include "vector.hpp"
#include "matrix.hpp"

namespace SDK {
    inline bool WorldToScreen(const Vector3& world, Vector2& screen, const Matrix4x4& viewMatrix, int screenWidth, int screenHeight) {
        float w = viewMatrix.m[0][3] * world.x + viewMatrix.m[1][3] * world.y + viewMatrix.m[2][3] * world.z + viewMatrix.m[3][3];
        if (w < 0.01f) return false;

        float x = viewMatrix.m[0][0] * world.x + viewMatrix.m[1][0] * world.y + viewMatrix.m[2][0] * world.z + viewMatrix.m[3][0];
        float y = viewMatrix.m[0][1] * world.x + viewMatrix.m[1][1] * world.y + viewMatrix.m[2][1] * world.z + viewMatrix.m[3][1];

        float invW = 1.0f / w;
        x *= invW;
        y *= invW;

        float xCenter = screenWidth / 2.0f;
        float yCenter = screenHeight / 2.0f;

        screen.x = xCenter + 0.5f * x * screenWidth + 0.5f;
        screen.y = yCenter - 0.5f * y * screenHeight + 0.5f;
        return true;
    }
}
