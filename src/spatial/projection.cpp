#include "spatial/projection.hpp"

#include <cmath>
#include <algorithm>

namespace izanagi::spatial {

bool ValidCamera(const CameraSnapshot& camera) noexcept
{
    if (!camera.valid || camera.viewport_width == 0 || camera.viewport_height == 0 ||
        camera.viewport_width > 16384 || camera.viewport_height > 16384 ||
        !Finite(camera.view_projection)) return false;
    bool nonzero = false;
    for (const auto& row : camera.view_projection.m)
        for (const float v : row) nonzero |= v != 0.0f;
    if (!nonzero) return false;
    double rows[4][4]{};
    for (int r = 0; r < 4; ++r) {
        double scale = 0.0;
        for (int c = 0; c < 4; ++c)
            scale = (std::max)(scale, std::fabs(static_cast<double>(
                camera.view_projection.m[r][c])));
        if (scale == 0.0) return false;
        for (int c = 0; c < 4; ++c)
            rows[r][c] = camera.view_projection.m[r][c] / scale;
    }
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        for (int row = col + 1; row < 4; ++row)
            if (std::fabs(rows[row][col]) > std::fabs(rows[pivot][col])) pivot = row;
        if (std::fabs(rows[pivot][col]) < 1.0e-6) return false;
        for (int c = col; c < 4; ++c)
            (std::swap)(rows[col][c], rows[pivot][c]);
        for (int row = col + 1; row < 4; ++row) {
            const double factor = rows[row][col] / rows[col][col];
            for (int c = col; c < 4; ++c)
                rows[row][c] -= factor * rows[col][c];
        }
    }
    return true;
}

ProjectionResult Project(Vec3 world, const CameraSnapshot& camera) noexcept
{
    ProjectionResult result;
    if (!Finite(world)) { result.status = ProjectionStatus::invalid_point; return result; }
    if (!ValidCamera(camera)) return result;
    const Vec4 clip = Multiply(camera.view_projection, {world.x, world.y, world.z, 1.0f});
    if (!Finite(clip)) { result.status = ProjectionStatus::invalid_point; return result; }
    if (clip.w <= k_clip_w_epsilon) {
        result.status = ProjectionStatus::behind_camera;
        return result;
    }
    const float inv_w = 1.0f / clip.w;
    const float x = clip.x * inv_w;
    const float y = clip.y * inv_w;
    result.depth = clip.z * inv_w;
    result.screen = {(x + 1.0f) * 0.5f * camera.viewport_width,
                     (1.0f - y) * 0.5f * camera.viewport_height};
    if (!std::isfinite(result.screen.x) || !std::isfinite(result.screen.y) ||
        !std::isfinite(result.depth)) {
        result = {};
        result.status = ProjectionStatus::invalid_point;
        return result;
    }
    result.status = x < -1.0f || x > 1.0f || y < -1.0f || y > 1.0f ||
                    result.depth < 0.0f || result.depth > 1.0f
        ? ProjectionStatus::outside_viewport : ProjectionStatus::visible;
    return result;
}

const char* ProjectionStatusName(ProjectionStatus status) noexcept
{
    switch (status) {
    case ProjectionStatus::visible: return "visible";
    case ProjectionStatus::outside_viewport: return "outside viewport";
    case ProjectionStatus::behind_camera: return "behind camera";
    case ProjectionStatus::invalid_camera: return "invalid camera";
    case ProjectionStatus::invalid_point: return "invalid point";
    }
    return "unknown";
}

}
