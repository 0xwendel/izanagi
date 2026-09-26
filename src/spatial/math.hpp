#pragma once

#include <cmath>

namespace izanagi::spatial {

struct Vec2 { float x{}, y{}; };
struct Vec3 { float x{}, y{}, z{}; };
struct Vec4 { float x{}, y{}, z{}, w{}; };
struct Matrix4x4 { float m[4][4]{}; };

constexpr float k_clip_w_epsilon = 1.0e-5f;

inline bool Finite(Vec3 v) noexcept
{ return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

inline bool Finite(Vec4 v) noexcept
{ return std::isfinite(v.x) && std::isfinite(v.y) &&
         std::isfinite(v.z) && std::isfinite(v.w); }

inline bool Finite(const Matrix4x4& a) noexcept
{
    for (const auto& row : a.m)
        for (const float v : row) if (!std::isfinite(v)) return false;
    return true;
}

inline Vec4 Multiply(const Matrix4x4& a, Vec4 v) noexcept
{
    return {
        a.m[0][0]*v.x + a.m[0][1]*v.y + a.m[0][2]*v.z + a.m[0][3]*v.w,
        a.m[1][0]*v.x + a.m[1][1]*v.y + a.m[1][2]*v.z + a.m[1][3]*v.w,
        a.m[2][0]*v.x + a.m[2][1]*v.y + a.m[2][2]*v.z + a.m[2][3]*v.w,
        a.m[3][0]*v.x + a.m[3][1]*v.y + a.m[3][2]*v.z + a.m[3][3]*v.w
    };
}

}
