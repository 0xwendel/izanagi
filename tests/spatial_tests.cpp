#include "spatial/projection.hpp"

#include <cmath>
#include <cstdio>
#include <limits>

using namespace izanagi::spatial;

namespace {

bool near(float a, float b) noexcept { return std::fabs(a - b) < 0.001f; }

bool check(bool ok, int line) noexcept
{
    if (!ok) std::fprintf(stderr, "spatial test failed at line %d\n", line);
    return ok;
}

#define CHECK(x) do { if (!check((x), __LINE__)) return 1; } while (false)

}

int main()
{
    CameraSnapshot camera;
    camera.valid = true;
    camera.viewport_width = 100;
    camera.viewport_height = 100;
    camera.view_projection.m[0][0] = 1.0f;
    camera.view_projection.m[1][1] = 1.0f;
    camera.view_projection.m[2][2] = 0.5f;
    camera.view_projection.m[2][3] = -0.1f;
    camera.view_projection.m[3][2] = 1.0f;

    const auto center = Project({0.0f, 0.0f, 1.0f}, camera);
    CHECK(center.status == ProjectionStatus::visible);
    CHECK(near(center.screen.x, 50.0f) && near(center.screen.y, 50.0f));
    CHECK(near(center.depth, 0.4f));
    CHECK(near(Project({-0.5f, 0.0f, 1.0f}, camera).screen.x, 25.0f));
    CHECK(near(Project({0.5f, 0.0f, 1.0f}, camera).screen.x, 75.0f));
    CHECK(near(Project({0.0f, 0.5f, 1.0f}, camera).screen.y, 25.0f));
    CHECK(near(Project({0.0f, -0.5f, 1.0f}, camera).screen.y, 75.0f));
    CHECK(Project({2.0f, 0.0f, 1.0f}, camera).status ==
          ProjectionStatus::outside_viewport);
    CHECK(Project({0.0f, 0.0f, -1.0f}, camera).status ==
          ProjectionStatus::behind_camera);
    CHECK(Project({0.0f, 0.0f, k_clip_w_epsilon}, camera).status ==
          ProjectionStatus::behind_camera);

    CameraSnapshot identity = camera;
    identity.view_projection = {};
    for (int i = 0; i < 4; ++i) identity.view_projection.m[i][i] = 1.0f;
    const auto translated = Project({-1.0f, 1.0f, 0.5f}, identity);
    CHECK(translated.status == ProjectionStatus::visible);
    CHECK(near(translated.screen.x, 0.0f) && near(translated.screen.y, 0.0f));

    camera.view_projection = {};
    CHECK(Project({0.0f, 0.0f, 1.0f}, camera).status ==
          ProjectionStatus::invalid_camera);
    camera.view_projection.m[0][0] = std::numeric_limits<float>::infinity();
    CHECK(Project({0.0f, 0.0f, 1.0f}, camera).status ==
          ProjectionStatus::invalid_camera);
    CHECK(Project({std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f},
                  identity).status == ProjectionStatus::invalid_point);
    identity.viewport_width = 0;
    CHECK(Project({0.0f, 0.0f, 1.0f}, identity).status ==
          ProjectionStatus::invalid_camera);
    return 0;
}
