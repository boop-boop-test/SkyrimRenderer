#include "SkyrimRenderer/CameraController.h"

#include <SKSE/SKSE.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace SkyrimRenderer {

namespace {

constexpr float kDegToRadF = 3.14159265f / 180.0f;

std::string Lower(const std::string& s) {
    std::string out = s;
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

struct Box {
    float min[3] = {1e9f, 1e9f, 1e9f};
    float max[3] = {-1e9f, -1e9f, -1e9f};
    bool Valid() const { return min[0] <= max[0]; }
    void Add(float x, float y, float z) {
        const float p[3] = {x, y, z};
        for (int a = 0; a < 3; ++a) {
            min[a] = std::min(min[a], p[a]);
            max[a] = std::max(max[a], p[a]);
        }
    }
    void Add(const Box& b) {
        if (!b.Valid()) return;
        Add(b.min[0], b.min[1], b.min[2]);
        Add(b.max[0], b.max[1], b.max[2]);
    }
    float Center(int a) const { return (min[a] + max[a]) * 0.5f; }
};

// Where the renderer draws each vertex of `mesh` under `pose`: the bone
// palette of MeshRenderer::UpdateBonePaletteForMesh, blended like the skinned
// vertex shader (weights not renormalised). False when that draw is not known
// here: no pose, not GPU-skinned, no CPU copy, or GPU morph deltas on top.
bool DrawnPositions(const LoadedMesh& mesh, const BonePoseMap* pose, std::vector<RE::NiPoint3>& out) {
    const auto& skin = mesh.skinData;
    const size_t n = mesh.bindPoseVertices.size();
    if (!pose || !mesh.isGPUSkinned || !skin.HasSkinning() || mesh.morphDeltaSRV) return false;
    if (n == 0 || n != mesh.vertexCount || skin.vertexWeights.size() < n) return false;
    if (skin.boneNames.size() > kMaxBones || skin.skinToBone.size() < skin.boneNames.size()) return false;

    std::vector<BoneTransform> palette(skin.boneNames.size());
    for (size_t b = 0; b < palette.size(); ++b) {
        const auto it = pose->find(skin.boneNames[b]);
        palette[b] = it != pose->end()
            ? it->second.Compose(skin.skinToBone[b])
            : skin.globalToSkin.Compose(skin.skinToBone[b]).Inverse().Compose(skin.skinToBone[b]);
    }
    out.resize(n);
    for (size_t v = 0; v < n; ++v) {
        const auto& w = skin.vertexWeights[v];
        float acc[3] = {0.0f, 0.0f, 0.0f};
        for (int k = 0; k < 4; ++k) {
            if (w.weights[k] == 0.0f || w.boneIndices[k] >= palette.size()) continue;
            float p[3];
            palette[w.boneIndices[k]].TransformPoint(mesh.bindPoseVertices[v].position, p);
            for (int a = 0; a < 3; ++a) acc[a] += w.weights[k] * p[a];
        }
        out[v] = {acc[0], acc[1], acc[2]};
    }
    return true;
}

}  // namespace

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
                                  const RE::NiPoint3* headBonePos,
                                  const BonePoseMap* pose) {
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
        case CameraPreset::HeadShot:
            FrameHeadShot(meshes, headBonePos, center, radius);
            break;
        case CameraPreset::FaceClose:
            if (!FrameFaceClose(meshes, pose)) FrameHeadShot(meshes, headBonePos, center, radius);
            break;
    }
}

void CameraController::FrameHeadShot(const std::vector<LoadedMesh>& meshes, const RE::NiPoint3* headBonePos,
                                     const RE::NiPoint3& center, float radius) {
    // Head and neck fill the frame. The FaceGen face shape runs from
    // the neck seam to the crown; hair / eyes may add a little on top
    // (capped so long hair does not pull the frame down the back).
    float fMin[3] = {1e9f, 1e9f, 1e9f}, fMax[3] = {-1e9f, -1e9f, -1e9f};
    bool foundFace = false;
    for (const auto& m : meshes) {
        if (m.shadingModel != ShadingModel::FaceGen) continue;
        for (int a = 0; a < 3; ++a) {
            fMin[a] = std::min(fMin[a], m.boundsMin[a]);
            fMax[a] = std::max(fMax[a], m.boundsMax[a]);
        }
        foundFace = true;
    }
    if (!foundFace) {
        // No face shape (helmet, creature): a fixed head-sized frame on
        // the head bone, else the top of the bounds.
        const float z = headBonePos ? headBonePos->z : center.z + radius * 0.7f;
        if (headBonePos) {
            orbitCenterX_ = headBonePos->x;
            orbitCenterY_ = headBonePos->y;
        }
        SetOrbit(0.0f, 0.0f, 45.0f, z);
        return;
    }

    const float faceHeight = fMax[2] - fMin[2];
    float top = fMax[2];
    for (const auto& m : meshes) {
        if (m.shadingModel != ShadingModel::HairTint && m.shadingModel != ShadingModel::EyeEnvMap) continue;
        top = std::max(top, std::min(m.boundsMax[2], fMax[2] + faceHeight * 0.15f));
    }
    const float bottom = fMin[2];
    const float height = top - bottom;
    const float width = fMax[0] - fMin[0];
    const float depth = fMax[1] - fMin[1];

    // Fit the taller of height / width into the vertical FOV (square
    // or wider targets get the same or more side room), measured at
    // the front of the face.
    // 12% room: the portrait is posed (idle) after framing on the bind pose.
    constexpr float kMargin = 1.12f;
    const float tanHalf = std::tan(fov_ * 0.5f * kDegToRadF);
    const float fit = std::max(height, width) * 0.5f * kMargin / std::max(tanHalf, 0.01f);

    orbitCenterX_ = (fMin[0] + fMax[0]) * 0.5f;
    orbitCenterY_ = headBonePos ? headBonePos->y : (fMin[1] + fMax[1]) * 0.5f;
    SetOrbit(0.0f, 0.0f, fit + depth * 0.5f, (bottom + top) * 0.5f);
}

bool CameraController::FrameFaceClose(const std::vector<LoadedMesh>& meshes, const BonePoseMap* pose) {
    // Brows to chin fill the frame, centred on the face. Everything is
    // measured on the head as drawn: under the scene's pose when every part
    // has GPU-skinned CPU data (so an idle set before this call is framed
    // where it puts the head), else on the load-time bounds (bind pose).
    //
    // Proportions, measured on 1165 FaceGen heads (Skyrim, DLC and Creation
    // Club NPCs, every race incl. children) and used only where the mesh
    // gives no feature:
    //  - no brow shape (Khajiit, children, Dremora, most Argonians): brow top
    //    = eye top + 1.12 eye heights (median of the 1007 heads with brows;
    //    median error 0.21 units, p90 0.56).
    //  - no posed face vertices: chin = mouth - 0.84 x eye-to-mouth (median
    //    of all 1165; human races 0.83-0.86, Khajiit 0.58, Argonian 0.91).
    constexpr float kBrowAboveEyeTop = 1.12f;   // eye heights
    constexpr float kChinBelowMouth  = 0.84f;   // eye-to-mouth distances
    // The lowest quarter of the face shape below the mouth is neck (the neck
    // seam to the chin is far more than that on every measured head).
    constexpr float kNeckBand = 0.25f;
    // Frame = 1.3 x brows-to-chin (11.5% above the brows and below the chin).
    // Against HeadShot that is 1.79x closer (median; p10 1.68, p90 1.93).
    constexpr float kMargin = 1.3f;

    // The parts: the FaceGen-shaded face (the largest such shape), the eyes
    // (eye shader), mouth and brows by head part name (FaceGen NIFs name each
    // shape after its head part; the Face preset finds the mouth the same way).
    const LoadedMesh* face = nullptr;
    const LoadedMesh* mouth = nullptr;
    std::vector<const LoadedMesh*> eyes, brows;
    for (const auto& m : meshes) {
        if (m.shadingModel == ShadingModel::FaceGen) {
            if (!face || m.vertexCount > face->vertexCount) face = &m;
            continue;
        }
        if (m.shadingModel == ShadingModel::EyeEnvMap) { eyes.push_back(&m); continue; }
        const std::string lower = Lower(m.name);
        if (!mouth && lower.find("mouth") != std::string::npos) mouth = &m;
        else if (lower.find("brow") != std::string::npos) brows.push_back(&m);
    }
    if (!face || eyes.empty() || !mouth) return false;

    std::vector<const LoadedMesh*> parts = {face, mouth};
    parts.insert(parts.end(), eyes.begin(), eyes.end());
    parts.insert(parts.end(), brows.begin(), brows.end());
    std::vector<std::vector<RE::NiPoint3>> drawn(parts.size());
    bool posed = pose != nullptr;
    for (size_t i = 0; posed && i < parts.size(); ++i) posed = DrawnPositions(*parts[i], pose, drawn[i]);

    auto boxOf = [&](const LoadedMesh* m) {
        Box b;
        const size_t i = static_cast<size_t>(std::find(parts.begin(), parts.end(), m) - parts.begin());
        if (posed) {
            for (const auto& p : drawn[i]) b.Add(p.x, p.y, p.z);
        } else {
            b.Add(m->boundsMin[0], m->boundsMin[1], m->boundsMin[2]);
            b.Add(m->boundsMax[0], m->boundsMax[1], m->boundsMax[2]);
        }
        return b;
    };

    const Box faceBox = boxOf(face);
    const Box mouthBox = boxOf(mouth);
    Box eyeBox;
    for (const auto* e : eyes) eyeBox.Add(boxOf(e));
    const float eyeZ = eyeBox.Center(2);
    const float eyeTop = eyeBox.max[2];
    const float eyeHeight = eyeBox.max[2] - eyeBox.min[2];
    const float mouthZ = mouthBox.Center(2);
    const float eyeToMouth = eyeZ - mouthZ;
    if (!faceBox.Valid() || eyeHeight <= 0.0f || eyeToMouth <= 0.0f) return false;   // not an upright face

    // Brows: a brow shape centred between the eyes and one eye-to-mouth
    // distance above them (measured brow tops reach 0.75 of it at most).
    Box browBox;
    for (const auto* b : brows) {
        const Box bb = boxOf(b);
        const float z = bb.Center(2);
        if (z > eyeZ && z < eyeZ + eyeToMouth) browBox.Add(bb);
    }
    const bool browShape = browBox.Valid();
    const float top = browShape ? browBox.max[2] : eyeTop + kBrowAboveEyeTop * eyeHeight;

    // Chin: the lowest point of the face in front of the line midway between
    // the chin's front and the neck's front. Below the chin the skin turns
    // back to the throat almost level, so the cut barely depends on where
    // between the two fronts it is drawn. Forward is +Y.
    float bottom = mouthZ - kChinBelowMouth * eyeToMouth;
    bool chinFromMesh = false;
    if (posed) {
        const auto& verts = drawn[0];
        const float neckTop = faceBox.min[2] + (mouthZ - faceBox.min[2]) * kNeckBand;
        float chinFront = -1e9f, neckFront = -1e9f;
        for (const auto& p : verts) {
            if (p.z < mouthZ) chinFront = std::max(chinFront, p.y);
            if (p.z <= neckTop) neckFront = std::max(neckFront, p.y);
        }
        if (chinFront > neckFront && neckFront > -1e8f) {
            const float cut = (chinFront + neckFront) * 0.5f;
            float lowest = mouthZ;
            for (const auto& p : verts) {
                if (p.z < mouthZ && p.y > cut) lowest = std::min(lowest, p.z);
            }
            if (lowest < mouthZ) {
                bottom = lowest;
                chinFromMesh = true;
            }
        }
    }
    const float height = top - bottom;
    if (height <= 0.0f) return false;

    // Fitted at the front of the eyes, the plane the brows and chin sit near.
    const float tanHalf = std::tan(fov_ * 0.5f * kDegToRadF);
    const float fit = height * 0.5f * kMargin / std::max(tanHalf, 0.01f);
    orbitCenterX_ = eyeBox.Center(0);
    orbitCenterY_ = faceBox.Center(1);
    SetOrbit(0.0f, 0.0f, fit + (eyeBox.max[1] - orbitCenterY_), (top + bottom) * 0.5f);

    SKSE::log::info("[SkyrimRenderer] FaceClose: brows {:.2f} ({}), chin {:.2f} ({}), eyes {:.2f}, mouth {:.2f}, "
                    "{} -> distance {:.1f}, focal {:.2f}",
                    top, browShape ? "brow shape" : "from eyes", bottom, chinFromMesh ? "face mesh" : "from mouth",
                    eyeZ, mouthZ, posed ? "posed" : "load bounds", distance_, focalHeight_);
    return true;
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
