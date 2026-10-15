#pragma once
#include <cmath>

struct Vector2 {
    float x = 0.0f, y = 0.0f;
};

struct Vector3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    Vector3 operator+(const Vector3& o) const { return { x + o.x, y + o.y, z + o.z }; }
    Vector3 operator-(const Vector3& o) const { return { x - o.x, y - o.y, z - o.z }; }
    Vector3 operator*(float f) const { return { x * f, y * f, z * f }; }

    float Distance(const Vector3& o) const {
        float dx = x - o.x, dy = y - o.y, dz = z - o.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
};

struct Vector4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
};
