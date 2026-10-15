// FrameToPoints solver test, outside the game: the real CameraController,
// checked through its own view / projection (Project), which the solver does
// not use (it solves the perspective algebraically).
// Built with the renderer (target skyrim-renderer-camera-test); run it from
// build/release-msvc.

#include "SkyrimRenderer/CameraController.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace SkyrimRenderer;

static int g_failures = 0;
#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (cond) {                                       \
            std::printf("  ok   " #cond "\n");            \
        } else {                                          \
            std::printf("  FAIL " #cond "  ");            \
            std::printf(__VA_ARGS__);                     \
            std::printf("\n");                            \
            ++g_failures;                                 \
        }                                                 \
    } while (0)

// brow top, right eye, left eye, mouth, chin of a face centred at (x, y, z),
// `s` = brows-to-chin height (game units).
static std::vector<RE::NiPoint3> Face(float x, float y, float z, float s) {
    return {
        {x, y + 0.10f * s, z + 0.50f * s},          // brow top
        {x + 0.22f * s, y, z + 0.28f * s},          // right eye (+X)
        {x - 0.22f * s, y, z + 0.28f * s},          // left eye
        {x, y + 0.05f * s, z - 0.18f * s},          // mouth
        {x, y + 0.02f * s, z - 0.50f * s},          // chin
    };
}

static bool Near(float a, float b, float tol = 1e-3f) { return std::fabs(a - b) <= tol; }

// The eyes' mean at (ax, ay), brows to chin `span` of the frame height.
static void Case(const char* name, CameraController& cam, std::vector<RE::NiPoint3> pts, float aspect, float ax,
                 float ay, float span, uint32_t axis = 0, bool setAngles = false, float yaw = 0, float pitch = 0) {
    std::printf("%s\n", name);
    PointsTarget t;
    t.points = std::move(pts);
    t.anchorMask = 0x6;
    t.anchorX = ax;
    t.anchorY = ay;
    t.spanMask = axis == 0 ? 0x11 : 0x6;  // brows-chin, or eye to eye
    t.spanAxis = axis;
    t.span = span;
    t.setAngles = setAngles;
    t.yaw = yaw;
    t.pitch = pitch;
    PointsResult r;
    const bool ok = cam.FrameToPoints(t, aspect, r);
    CHECK(ok, "solver refused");
    if (!ok) return;

    // Independent measurement through Project.
    float rx, ry, lx, ly, bx, by, cx, cy;
    cam.Project(t.points[1], aspect, rx, ry);
    cam.Project(t.points[2], aspect, lx, ly);
    cam.Project(t.points[0], aspect, bx, by);
    cam.Project(t.points[4], aspect, cx, cy);
    const float want = span / cam.GetFramingScale();
    // The anchor is the eyes' 3D mean; its projection is what we pinned.
    RE::NiPoint3 mid = (t.points[1] + t.points[2]) * 0.5f;
    float mx, my;
    cam.Project(mid, aspect, mx, my);
    CHECK(Near(mx, ax) && Near(my, ay), "anchor at (%.4f, %.4f), want (%.4f, %.4f)", mx, my, ax, ay);
    if (axis == 0) {
        CHECK(Near(cy - by, want), "brows-chin %.4f, want %.4f", cy - by, want);
    } else {
        CHECK(Near(std::fabs(lx - rx), want), "eye to eye %.4f, want %.4f", std::fabs(lx - rx), want);
    }
    CHECK(Near(r.anchorX, mx) && Near(r.anchorY, my), "reported anchor (%.4f, %.4f)", r.anchorX, r.anchorY);
    CHECK(r.spanReached, "");
    if (setAngles) CHECK(Near(cam.GetYaw(), yaw, 1e-4f) && Near(cam.GetPitch(), pitch, 1e-4f), "angles %.3f %.3f",
                         cam.GetYaw(), cam.GetPitch());
    std::printf("    camera: yaw %.1f pitch %.1f distance %.2f focal %.2f; eyes (%.4f, %.4f), brows-chin %.4f\n",
                cam.GetYaw(), cam.GetPitch(), cam.GetDistance(), cam.GetFocalHeight(), mx, my, cy - by);
}

int main() {
    CameraController cam;
    cam.SetFOV(22.0f);

    // Heads of different size, height and depth: the same framing for all.
    Case("adult at 120 high, 22 deg", cam, Face(0, 5, 120, 18), 1.0f, 0.5f, 0.42f, 0.55f);
    Case("child (smaller, lower)", cam, Face(1, 3, 85, 13), 1.0f, 0.5f, 0.42f, 0.55f);
    Case("tall big head, off-centre", cam, Face(-4, 9, 135, 23), 1.0f, 0.5f, 0.42f, 0.55f);
    Case("anchor off-centre (0.35, 0.3)", cam, Face(0, 5, 120, 18), 1.0f, 0.35f, 0.3f, 0.4f);
    Case("wide target 16:9", cam, Face(0, 5, 120, 18), 16.0f / 9.0f, 0.5f, 0.42f, 0.55f);
    Case("horizontal span: eyes 30% apart", cam, Face(0, 5, 120, 18), 1.0f, 0.5f, 0.45f, 0.3f, 1);
    Case("yaw 35, pitch -10 set", cam, Face(0, 5, 120, 18), 1.0f, 0.5f, 0.42f, 0.55f, 0, true, 35.0f, -10.0f);
    Case("keeps yaw 35 / pitch -10 next time", cam, Face(2, 4, 110, 17), 1.0f, 0.5f, 0.42f, 0.55f);
    cam.SetFOV(50.0f);
    Case("wide lens 50 deg", cam, Face(0, 5, 120, 18), 1.0f, 0.5f, 0.42f, 0.55f, 0, true, 0, 0);
    cam.SetFramingScale(1.25f);
    Case("framing scale 1.25 (span / 1.25)", cam, Face(0, 5, 120, 18), 1.0f, 0.5f, 0.42f, 0.55f);
    cam.SetFramingScale(1.0f);
    cam.SetFOV(22.0f);

    std::printf("refusals\n");
    PointsResult r;
    PointsTarget t;
    CHECK(!cam.FrameToPoints(t, 1.0f, r), "no points accepted");
    t.points = {{0, 0, 100}};
    CHECK(!cam.FrameToPoints(t, 1.0f, r), "one point has no extent");
    t.points = {{0, 0, 100}, {0, 0, 100}};
    CHECK(!cam.FrameToPoints(t, 1.0f, r), "coincident points");
    t.points = Face(0, 5, 120, 18);
    t.span = NAN;
    CHECK(!cam.FrameToPoints(t, 1.0f, r), "NaN span");
    t.span = 0.5f;
    t.points[0].z = INFINITY;
    CHECK(!cam.FrameToPoints(t, 1.0f, r), "non-finite point");

    std::printf("unreachable span\n");
    t.points = Face(0, 5, 120, 18);
    t.anchorMask = 0x6;
    t.spanMask = 0x11;
    t.span = 40.0f;  // 40 frame heights: closer than the nearest point allows
    CHECK(cam.FrameToPoints(t, 1.0f, r) && !r.spanReached, "reached %d", r.spanReached);

    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
