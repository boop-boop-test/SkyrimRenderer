#include "SkyrimRenderer/CameraController.h"

#include <algorithm>
#include <cmath>

namespace SkyrimRenderer {

void CameraController::SetOrbit(float yaw, float pitch, float distance, float focalHeight) {
    yaw_ = yaw;
    pitch_ = std::clamp(pitch, -89.0f, 89.0f);
    distance_ = std::max(distance, 10.0f);
    focalHeight_ = focalHeight;
    dirty_ = true;
}

void CameraController::SetFOV(float degrees) {
    fov_ = std::clamp(degrees, 10.0f, 120.0f);
    dirty_ = true;
}

void CameraController::SetPreset(CameraPreset preset, const std::vector<LoadedMesh>& meshes,
                                  RE::NiNode* sceneRoot,
                                  const RE::NiPoint3* headBonePos) {
    RE::NiPoint3 center;
    float radius;

    if (!meshes.empty()) {
        float minB[3] = {1e9f, 1e9f, 1e9f}, maxB[3] = {-1e9f, -1e9f, -1e9f};
        for (const auto& m : meshes) {
            for (int a = 0; a < 3; ++a) {
                if (m.boundsMin[a] < minB[a]) minB[a] = m.boundsMin[a];
                if (m.boundsMax[a] > maxB[a]) maxB[a] = m.boundsMax[a];
            }
        }
        center.x = (minB[0] + maxB[0]) * 0.5f;
        center.y = (minB[1] + maxB[1]) * 0.5f;
        center.z = (minB[2] + maxB[2]) * 0.5f;
        float dx = maxB[0] - minB[0], dy = maxB[1] - minB[1], dz = maxB[2] - minB[2];
        radius = sqrtf(dx*dx + dy*dy + dz*dz) * 0.5f;
    } else {
        ComputeSceneBounds(sceneRoot, center, radius);
    }

    if (radius < 1.0f) radius = 100.0f;

    // Update cached orbit center from the computed bounds
    orbitCenterX_ = center.x;
    orbitCenterY_ = center.y;
    orbitCenterValid_ = true;

    switch (preset) {
        case CameraPreset::FullBody:
            SetOrbit(0.0f, 5.0f, radius * 2.5f, center.z);
            break;
        case CameraPreset::UpperBody:
            SetOrbit(0.0f, 5.0f, radius * 1.5f, center.z + radius * 0.3f);
            break;
        case CameraPreset::Face: {
            // Pivot on head parts for accurate face framing.
            //   Best:     eye + mouth meshes for X/Z, head bone for Y (depth)
            //   Fallback: head bone alone for all axes
            //   Last:     bounds heuristic
            RE::NiPoint3 eyeCenter{0, 0, 0};
            RE::NiPoint3 mouthCenter{0, 0, 0};
            bool foundEyes = false, foundMouth = false;

            for (const auto& m : meshes) {
                float cx = (m.boundsMin[0] + m.boundsMax[0]) * 0.5f;
                float cy = (m.boundsMin[1] + m.boundsMax[1]) * 0.5f;
                float cz = (m.boundsMin[2] + m.boundsMax[2]) * 0.5f;

                if (m.shadingModel == ShadingModel::EyeEnvMap) {
                    if (!foundEyes) {
                        eyeCenter = {cx, cy, cz};
                        foundEyes = true;
                    } else {
                        // Second eye — average with first
                        eyeCenter.x = (eyeCenter.x + cx) * 0.5f;
                        eyeCenter.y = (eyeCenter.y + cy) * 0.5f;
                        eyeCenter.z = (eyeCenter.z + cz) * 0.5f;
                    }
                }

                if (!foundMouth && !m.name.empty()) {
                    std::string lower = m.name;
                    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (lower.find("mouth") != std::string::npos) {
                        mouthCenter = {cx, cy, cz};
                        foundMouth = true;
                    }
                }
            }

            if (foundEyes && foundMouth && headBonePos) {
                // X/Z: midpoint between eyes and mouth (center of face)
                // Y (depth): head bone (true center of skull)
                orbitCenterX_ = (eyeCenter.x + mouthCenter.x) * 0.5f;
                orbitCenterY_ = headBonePos->y;
                SetOrbit(0.0f, 5.0f, radius * 0.6f,
                         (eyeCenter.z + mouthCenter.z) * 0.5f);
            } else if (headBonePos) {
                // Head bone fallback for all axes
                orbitCenterX_ = headBonePos->x;
                orbitCenterY_ = headBonePos->y;
                SetOrbit(0.0f, 5.0f, radius * 0.6f, headBonePos->z);
            } else {
                // Last resort: estimate from bounds
                SetOrbit(0.0f, 5.0f, radius * 0.6f, center.z + radius * 0.7f);
            }
            break;
        }
    }
}

void CameraController::UpdateOrbitCenter(const std::vector<LoadedMesh>& meshes, RE::NiNode* sceneRoot) {
    if (!meshes.empty()) {
        float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
        for (const auto& m : meshes) {
            if (m.boundsMin[0] < minX) minX = m.boundsMin[0];
            if (m.boundsMin[1] < minY) minY = m.boundsMin[1];
            if (m.boundsMax[0] > maxX) maxX = m.boundsMax[0];
            if (m.boundsMax[1] > maxY) maxY = m.boundsMax[1];
        }
        orbitCenterX_ = (minX + maxX) * 0.5f;
        orbitCenterY_ = (minY + maxY) * 0.5f;
    } else if (sceneRoot) {
        RE::NiPoint3 sceneCenter;
        float sceneRadius;
        ComputeSceneBounds(sceneRoot, sceneCenter, sceneRadius);
        orbitCenterX_ = sceneCenter.x;
        orbitCenterY_ = sceneCenter.y;
    } else {
        orbitCenterX_ = 0.0f;
        orbitCenterY_ = 0.0f;
    }
    orbitCenterValid_ = true;
    dirty_ = true;  // Force recompute with new center
}

void CameraController::Pan(float dx, float dy) {
    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    float yawRad = yaw_ * kDegToRad;

    // dx shifts along camera's right vector (projected to horizontal plane)
    orbitCenterX_ += cosf(yawRad) * dx;
    orbitCenterY_ -= sinf(yawRad) * dx;

    // dy shifts vertically (focal height)
    focalHeight_ += dy;

    dirty_ = true;
}

void CameraController::Update() {
    if (!dirty_) return;

    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;

    float yawRad = yaw_ * kDegToRad;
    float pitchRad = pitch_ * kDegToRad;

    float cosP = cosf(pitchRad);
    float sinP = sinf(pitchRad);
    float cosY = cosf(yawRad);
    float sinY = sinf(yawRad);

    cameraTarget_ = RE::NiPoint3(orbitCenterX_, orbitCenterY_, focalHeight_);
    cameraPosition_ = cameraTarget_ + RE::NiPoint3(
        sinY * cosP * distance_,
        cosY * cosP * distance_,
        sinP * distance_
    );

    dirty_ = false;
}

DirectX::XMMATRIX CameraController::GetViewProjection(float aspectRatio) const {
    using namespace DirectX;

    XMVECTOR eyePos = XMVectorSet(cameraPosition_.x, cameraPosition_.y, cameraPosition_.z, 1.0f);
    XMVECTOR targetPos = XMVectorSet(cameraTarget_.x, cameraTarget_.y, cameraTarget_.z, 1.0f);
    XMVECTOR upDir = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);

    XMMATRIX view = XMMatrixLookAtRH(eyePos, targetPos, upDir);
    float fovRad = fov_ * XM_PI / 180.0f;
    XMMATRIX proj = XMMatrixPerspectiveFovRH(fovRad, aspectRatio, 0.1f, 10000.0f);

    return view * proj;
}

void CameraController::ComputeSceneBounds(RE::NiNode* sceneRoot,
                                           RE::NiPoint3& outCenter, float& outRadius) const {
    outCenter = RE::NiPoint3(0.0f, 0.0f, 85.0f);
    outRadius = 100.0f;

    if (!sceneRoot) return;

    const auto& bound = sceneRoot->worldBound;
    if (bound.radius > 0.0f) {
        outCenter = bound.center;
        outRadius = bound.radius;
    }
}

} // namespace SkyrimRenderer
