#include "SkyrimRenderer/SplineDecompressor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
namespace SkyrimRenderer {

// ── Constants ───────────────────────────────────────────────────────────────

static constexpr float PI = 3.14159265358979323846f;
static constexpr float HALF_PI = PI * 0.5f;
static constexpr float QUARTER_PI = PI * 0.25f;

// ── Quaternion Decompression ────────────────────────────────────────────────

void DecompressQuat32(uint32_t packed, float out[4]) {
    // Spherical coordinate packing (18 bits phiTheta, 10 bits R, 4 bits signs)
    uint32_t phiThetaRaw = packed & 0x3FFFF;       // bits [17:0]
    uint32_t rRaw = (packed >> 18) & 0x3FF;          // bits [27:18]
    uint32_t signBits = (packed >> 28) & 0x0F;        // bits [31:28]

    float R = 1.0f - (static_cast<float>(rRaw) / 1023.0f) * (static_cast<float>(rRaw) / 1023.0f);

    float phiThetaF = static_cast<float>(phiThetaRaw);
    float phi = std::floor(std::sqrt(phiThetaF));
    phi = phi * (HALF_PI / 511.0f);

    float theta = 0.0f;
    float phiFloor = std::floor(std::sqrt(static_cast<float>(phiThetaRaw)));
    if (phiFloor > 0.0f) {
        float phiFloorSq = phiFloor * phiFloor;
        theta = QUARTER_PI * (phiThetaF - phiFloorSq) / phiFloor;
    }

    float magnitude = std::sqrt(std::max(0.0f, 1.0f - R * R));
    float sinPhi = std::sin(phi);
    float cosPhi = std::cos(phi);
    float sinTheta = std::sin(theta);
    float cosTheta = std::cos(theta);

    out[0] = sinPhi * cosTheta * magnitude;
    out[1] = sinPhi * sinTheta * magnitude;
    out[2] = cosPhi * magnitude;
    out[3] = R;

    // Apply sign bits
    if (signBits & 0x01) out[0] = -out[0];
    if (signBits & 0x02) out[1] = -out[1];
    if (signBits & 0x04) out[2] = -out[2];
    if (signBits & 0x08) out[3] = -out[3];
}

void DecompressQuat40(const uint8_t bytes[5], float out[4]) {
    // 5 bytes = 40 bits
    // 3 x 12-bit components, 2-bit dropped index, 1-bit sign, 1 bit unused
    uint64_t packed = 0;
    for (int i = 0; i < 5; ++i) {
        packed |= static_cast<uint64_t>(bytes[i]) << (i * 8);
    }

    // Extract three 12-bit values
    int raw[3];
    raw[0] = static_cast<int>((packed >> 0) & 0xFFF);   // bits [11:0]
    raw[1] = static_cast<int>((packed >> 12) & 0xFFF);  // bits [23:12]
    raw[2] = static_cast<int>((packed >> 24) & 0xFFF);  // bits [35:24]

    // Dropped component index and sign
    int droppedIndex = static_cast<int>((packed >> 36) & 0x03);  // bits [37:36]
    int droppedSign = static_cast<int>((packed >> 38) & 0x01);   // bit 38

    // Dequantize: map [0, 4095] → [-sqrt(2)/2, +sqrt(2)/2]
    // offset = 2047, scale = sqrt(2)/2 / 2047 ≈ 0.000345436
    static constexpr float kScale40 = 0.000345436f;
    static constexpr float kOffset40 = 2047.0f;
    float vals[3];
    for (int i = 0; i < 3; ++i) {
        vals[i] = (static_cast<float>(raw[i]) - kOffset40) * kScale40;
    }

    // Reconstruct the 4th component
    float sumSq = vals[0] * vals[0] + vals[1] * vals[1] + vals[2] * vals[2];
    float dropped = std::sqrt(std::max(0.0f, 1.0f - sumSq));
    if (droppedSign) dropped = -dropped;

    // Place into correct position
    float quat[4];
    int vi = 0;
    for (int i = 0; i < 4; ++i) {
        if (i == droppedIndex) {
            quat[i] = dropped;
        } else {
            quat[i] = vals[vi++];
        }
    }

    out[0] = quat[0];
    out[1] = quat[1];
    out[2] = quat[2];
    out[3] = quat[3];
}

void DecompressQuat48(const uint8_t bytes[6], float out[4]) {
    // 3 x uint16 — use unsigned to avoid implementation-defined right-shift behavior
    uint16_t raw[3];
    std::memcpy(&raw[0], bytes + 0, 2);
    std::memcpy(&raw[1], bytes + 2, 2);
    std::memcpy(&raw[2], bytes + 4, 2);

    // Extract dropped index: bit 15 of X (low bit), bit 15 of Y (high bit)
    int droppedIndex = ((raw[0] >> 15) & 1) | (((raw[1] >> 15) & 1) << 1);

    // Extract sign of dropped component from bit 15 of Z
    int droppedSign = (raw[2] >> 15) & 1;

    // Mask off the flag bits — each value is 15 bits of unsigned data.
    // Dequantize: map [0, 32767] → [-sqrt(2)/2, +sqrt(2)/2]
    // offset = 2^14 - 1 = 16383, scale = sqrt(2)/2 / 16383 ≈ 0.000043161
    static constexpr float kScale48 = 0.000043161f;
    static constexpr float kOffset48 = 16383.0f;
    float vals[3];
    vals[0] = (static_cast<float>(raw[0] & 0x7FFF) - kOffset48) * kScale48;
    vals[1] = (static_cast<float>(raw[1] & 0x7FFF) - kOffset48) * kScale48;
    vals[2] = (static_cast<float>(raw[2] & 0x7FFF) - kOffset48) * kScale48;

    // Reconstruct dropped component
    float sumSq = vals[0] * vals[0] + vals[1] * vals[1] + vals[2] * vals[2];
    float dropped = std::sqrt(std::max(0.0f, 1.0f - sumSq));
    if (droppedSign) dropped = -dropped;

    float quat[4];
    int vi = 0;
    for (int i = 0; i < 4; ++i) {
        if (i == droppedIndex) {
            quat[i] = dropped;
        } else {
            quat[i] = vals[vi++];
        }
    }

    out[0] = quat[0];
    out[1] = quat[1];
    out[2] = quat[2];
    out[3] = quat[3];
}

// ── B-Spline Evaluation ─────────────────────────────────────────────────────

int FindKnotSpan(int degree, float frame, int controlPointCount, const uint8_t* knots) {
    if (frame >= static_cast<float>(knots[controlPointCount])) {
        return controlPointCount - 1;
    }

    int low = degree;
    int high = controlPointCount;

    while (low < high) {
        int mid = (low + high) / 2;
        if (frame < static_cast<float>(knots[mid])) {
            high = mid;
        } else if (frame >= static_cast<float>(knots[mid + 1])) {
            low = mid + 1;
        } else {
            return mid;
        }
    }

    return low;
}

float EvaluateScalarSpline(int degree, float frame, int controlPointCount,
                           const uint8_t* knots, const float* controlPoints) {
    if (controlPointCount <= 0) return 0.0f;
    if (controlPointCount == 1) return controlPoints[0];
    if (degree > 4) degree = 4;  // guard against OOB on N[5]

    int span = FindKnotSpan(degree, frame, controlPointCount, knots);

    // Cox-de Boor basis evaluation (iterative)
    float N[5] = {};  // max degree 4
    N[0] = 1.0f;

    for (int i = 1; i <= degree; ++i) {
        for (int j = i - 1; j >= 0; --j) {
            float knotLeft = static_cast<float>(knots[span - j]);
            float knotRight = static_cast<float>(knots[span + i - j]);
            float denom = knotRight - knotLeft;

            float alpha = (denom > 0.0f) ? (frame - knotLeft) / denom : 0.0f;
            float tmp = N[j] * alpha;
            N[j + 1] += N[j] - tmp;
            N[j] = tmp;
        }
    }

    // Weighted sum of control points
    float result = 0.0f;
    for (int i = 0; i <= degree; ++i) {
        int cpIdx = span - i;
        if (cpIdx >= 0 && cpIdx < controlPointCount) {
            result += controlPoints[cpIdx] * N[i];
        }
    }

    return result;
}

void EvaluateQuaternionSpline(int degree, float frame, int controlPointCount,
                              const uint8_t* knots,
                              const HKXSplineTrack::Quat* controlPoints,
                              float out[4]) {
    if (controlPointCount <= 0) {
        out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f;
        return;
    }
    if (controlPointCount == 1) {
        out[0] = controlPoints[0].x;
        out[1] = controlPoints[0].y;
        out[2] = controlPoints[0].z;
        out[3] = controlPoints[0].w;
        return;
    }

    if (degree > 4) degree = 4;  // guard against OOB on N[5]

    int span = FindKnotSpan(degree, frame, controlPointCount, knots);

    // Evaluate basis functions
    float N[5] = {};
    N[0] = 1.0f;

    for (int i = 1; i <= degree; ++i) {
        for (int j = i - 1; j >= 0; --j) {
            float knotLeft = static_cast<float>(knots[span - j]);
            float knotRight = static_cast<float>(knots[span + i - j]);
            float denom = knotRight - knotLeft;

            float alpha = (denom > 0.0f) ? (frame - knotLeft) / denom : 0.0f;
            float tmp = N[j] * alpha;
            N[j + 1] += N[j] - tmp;
            N[j] = tmp;
        }
    }

    // Weighted sum of quaternion control points with hemisphere correction.
    // q and -q represent the same rotation — ensure all contributing control points
    // are in the same hemisphere as the first one to avoid cancellation/flips.
    float qx = 0, qy = 0, qz = 0, qw = 0;

    // Find the first valid control point to use as reference hemisphere
    float refX = 0, refY = 0, refZ = 0, refW = 1;
    for (int i = 0; i <= degree; ++i) {
        int cpIdx = span - i;
        if (cpIdx >= 0 && cpIdx < controlPointCount && N[i] > 0.0f) {
            refX = controlPoints[cpIdx].x;
            refY = controlPoints[cpIdx].y;
            refZ = controlPoints[cpIdx].z;
            refW = controlPoints[cpIdx].w;
            break;
        }
    }

    for (int i = 0; i <= degree; ++i) {
        int cpIdx = span - i;
        if (cpIdx >= 0 && cpIdx < controlPointCount) {
            float cx = controlPoints[cpIdx].x;
            float cy = controlPoints[cpIdx].y;
            float cz = controlPoints[cpIdx].z;
            float cw = controlPoints[cpIdx].w;

            // Flip to same hemisphere as reference
            float dot = cx * refX + cy * refY + cz * refZ + cw * refW;
            if (dot < 0.0f) {
                cx = -cx; cy = -cy; cz = -cz; cw = -cw;
            }

            qx += cx * N[i];
            qy += cy * N[i];
            qz += cz * N[i];
            qw += cw * N[i];
        }
    }

    // Normalize
    float len = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
    if (len > 1e-8f) {
        float inv = 1.0f / len;
        out[0] = qx * inv;
        out[1] = qy * inv;
        out[2] = qz * inv;
        out[3] = qw * inv;
    } else {
        out[0] = out[1] = out[2] = 0.0f;
        out[3] = 1.0f;
    }
}

// ── Track Sampling ──────────────────────────────────────────────────────────

void SampleTrack(const HKXSplineTrack& track, float frame,
                 float outPos[3], float outRot[4], float outScale[3]) {
    // Position
    for (int c = 0; c < 3; ++c) {
        switch (track.posType[c]) {
        case HKXTrackType::Dynamic:
            outPos[c] = EvaluateScalarSpline(
                track.posDegree, frame,
                static_cast<int>(track.posControlPoints[c].size()),
                track.posKnots.data(),
                track.posControlPoints[c].data());
            break;
        case HKXTrackType::Static:
            outPos[c] = track.posStatic[c];
            break;
        case HKXTrackType::Identity:
        default:
            outPos[c] = 0.0f;
            break;
        }
    }

    // Rotation
    switch (track.rotType) {
    case HKXTrackType::Dynamic:
        EvaluateQuaternionSpline(
            track.rotDegree, frame,
            static_cast<int>(track.rotControlPoints.size()),
            track.rotKnots.data(),
            track.rotControlPoints.data(),
            outRot);
        break;
    case HKXTrackType::Static:
        outRot[0] = track.rotStatic[0];
        outRot[1] = track.rotStatic[1];
        outRot[2] = track.rotStatic[2];
        outRot[3] = track.rotStatic[3];
        break;
    case HKXTrackType::Identity:
    default:
        outRot[0] = outRot[1] = outRot[2] = 0.0f;
        outRot[3] = 1.0f;
        break;
    }

    // Scale
    for (int c = 0; c < 3; ++c) {
        switch (track.scaleType[c]) {
        case HKXTrackType::Dynamic:
            outScale[c] = EvaluateScalarSpline(
                track.scaleDegree, frame,
                static_cast<int>(track.scaleControlPoints[c].size()),
                track.scaleKnots.data(),
                track.scaleControlPoints[c].data());
            break;
        case HKXTrackType::Static:
            outScale[c] = track.scaleStatic[c];
            break;
        case HKXTrackType::Identity:
        default:
            outScale[c] = 1.0f;
            break;
        }
    }
}

// ── Block Parsing ───────────────────────────────────────────────────────────

namespace {

class BlockReader {
public:
    BlockReader(const uint8_t* data, size_t size)
        : data_(data), size_(size), pos_(0) {}

    size_t Position() const { return pos_; }
    bool IsValid() const { return pos_ <= size_; }
    void SeekTo(size_t pos) { pos_ = pos; }

    void Align(size_t alignment) {
        size_t mod = pos_ % alignment;
        if (mod != 0) pos_ += alignment - mod;
    }

    uint8_t ReadU8() {
        if (pos_ + 1 > size_) { pos_ = size_ + 1; return 0; }
        return data_[pos_++];
    }

    uint16_t ReadU16() {
        if (pos_ + 2 > size_) { pos_ = size_ + 1; return 0; }
        uint16_t v;
        std::memcpy(&v, data_ + pos_, 2);
        pos_ += 2;
        return v;
    }

    int16_t ReadS16() { return static_cast<int16_t>(ReadU16()); }

    uint32_t ReadU32() {
        if (pos_ + 4 > size_) { pos_ = size_ + 1; return 0; }
        uint32_t v;
        std::memcpy(&v, data_ + pos_, 4);
        pos_ += 4;
        return v;
    }

    float ReadFloat() {
        if (pos_ + 4 > size_) { pos_ = size_ + 1; return 0.0f; }
        float v;
        std::memcpy(&v, data_ + pos_, 4);
        pos_ += 4;
        return v;
    }

    void ReadBytes(void* dst, size_t count) {
        if (pos_ + count > size_) { pos_ = size_ + 1; return; }
        std::memcpy(dst, data_ + pos_, count);
        pos_ += count;
    }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_;
};

void ReadVectorTrack(BlockReader& br, HKXSplineTrack& track,
                     HKXTrackType types[3], HKXQuantType quant,
                     float defaultVal,
                     uint16_t& outNumCP, uint8_t& outDegree,
                     std::vector<uint8_t>& outKnots,
                     float bbox[3][2], float staticVals[3],
                     std::vector<float> controlPoints[3]) {
    // Check if any component is dynamic
    bool hasDynamic = false;
    for (int c = 0; c < 3; ++c) {
        if (types[c] == HKXTrackType::Dynamic) { hasDynamic = true; break; }
    }

    if (!hasDynamic) {
        // All static or identity — read static values
        for (int c = 0; c < 3; ++c) {
            if (types[c] == HKXTrackType::Static) {
                staticVals[c] = br.ReadFloat();
            } else {
                staticVals[c] = defaultVal;
            }
        }
        outNumCP = 0;
        return;
    }

    // Dynamic: read spline header
    outNumCP = br.ReadU16();  // numItems (control points - 1)
    outDegree = br.ReadU8();
    uint16_t numControlPoints = outNumCP + 1;
    uint32_t knotCount = outNumCP + outDegree + 2;

    outKnots.resize(knotCount);
    for (uint32_t i = 0; i < knotCount; ++i) {
        outKnots[i] = br.ReadU8();
    }

    br.Align(4);

    // Per-component bounding box or static value
    for (int c = 0; c < 3; ++c) {
        if (types[c] == HKXTrackType::Dynamic) {
            bbox[c][0] = br.ReadFloat();  // min
            bbox[c][1] = br.ReadFloat();  // max
        } else if (types[c] == HKXTrackType::Static) {
            staticVals[c] = br.ReadFloat();
        } else {
            staticVals[c] = defaultVal;
        }
    }

    // Read quantized control points
    for (uint16_t cp = 0; cp < numControlPoints; ++cp) {
        for (int c = 0; c < 3; ++c) {
            if (types[c] != HKXTrackType::Dynamic) continue;

            float val;
            float mn = bbox[c][0];
            float mx = bbox[c][1];

            if (quant == HKXQuantType::QT_8bit) {
                uint8_t raw = br.ReadU8();
                val = mn + (mx - mn) * (static_cast<float>(raw) / 255.0f);
            } else {  // QT_16bit
                uint16_t raw = br.ReadU16();
                val = mn + (mx - mn) * (static_cast<float>(raw) / 65535.0f);
            }

            controlPoints[c].push_back(val);
        }
    }
}

void ReadRotationTrack(BlockReader& br, HKXSplineTrack& track) {
    if (track.rotType == HKXTrackType::Identity) {
        track.rotStatic[0] = track.rotStatic[1] = track.rotStatic[2] = 0.0f;
        track.rotStatic[3] = 1.0f;
        return;
    }

    auto readOneQuat = [&](HKXQuantType quant, float out[4]) {
        switch (quant) {
        case HKXQuantType::QT_40bit: {
            uint8_t bytes[5];
            br.ReadBytes(bytes, 5);
            DecompressQuat40(bytes, out);
            break;
        }
        case HKXQuantType::QT_48bit: {
            uint8_t bytes[6];
            br.ReadBytes(bytes, 6);
            DecompressQuat48(bytes, out);
            break;
        }
        case HKXQuantType::QT_32bit: {
            uint32_t packed = br.ReadU32();
            DecompressQuat32(packed, out);
            break;
        }
        case HKXQuantType::QT_24bit: {
            // 3 x 8-bit components (x, y, z), W is always the dropped component (positive)
            uint8_t a = br.ReadU8();
            uint8_t b = br.ReadU8();
            uint8_t c = br.ReadU8();
            static constexpr float kScale24 = 0.0055242f;  // sqrt(2)/2 / 128
            static constexpr float kOffset24 = 127.0f;
            out[0] = (static_cast<float>(a) - kOffset24) * kScale24;
            out[1] = (static_cast<float>(b) - kOffset24) * kScale24;
            out[2] = (static_cast<float>(c) - kOffset24) * kScale24;
            float sumSq = out[0]*out[0] + out[1]*out[1] + out[2]*out[2];
            out[3] = std::sqrt(std::max(0.0f, 1.0f - sumSq));
            break;
        }
        case HKXQuantType::QT_16bitQuat: {
            // 4 x int16 → normalize to unit quaternion
            // Each component: int16 / 32767 maps to [-1, 1]
            int16_t x = br.ReadS16();
            int16_t y = br.ReadS16();
            int16_t z = br.ReadS16();
            int16_t w = br.ReadS16();
            out[0] = static_cast<float>(x) / 32767.0f;
            out[1] = static_cast<float>(y) / 32767.0f;
            out[2] = static_cast<float>(z) / 32767.0f;
            out[3] = static_cast<float>(w) / 32767.0f;
            // Normalize
            float len = std::sqrt(out[0]*out[0] + out[1]*out[1] + out[2]*out[2] + out[3]*out[3]);
            if (len > 1e-8f) {
                float inv = 1.0f / len;
                out[0] *= inv; out[1] *= inv; out[2] *= inv; out[3] *= inv;
            } else {
                out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f;
            }
            break;
        }
        case HKXQuantType::QT_Uncompressed: {
            out[0] = br.ReadFloat();
            out[1] = br.ReadFloat();
            out[2] = br.ReadFloat();
            out[3] = br.ReadFloat();
            break;
        }
        default:
            out[0] = out[1] = out[2] = 0.0f;
            out[3] = 1.0f;
            break;
        }
    };

    if (track.rotType == HKXTrackType::Static) {
        readOneQuat(track.rotQuant, track.rotStatic);
        return;
    }

    // Dynamic rotation
    track.rotNumControlPoints = br.ReadU16();
    track.rotDegree = br.ReadU8();
    uint16_t numCP = track.rotNumControlPoints + 1;
    uint32_t knotCount = track.rotNumControlPoints + track.rotDegree + 2;

    track.rotKnots.resize(knotCount);
    for (uint32_t i = 0; i < knotCount; ++i) {
        track.rotKnots[i] = br.ReadU8();
    }

    // Alignment depends on quantization type
    switch (track.rotQuant) {
    case HKXQuantType::QT_48bit:
    case HKXQuantType::QT_16bitQuat:
        br.Align(2);
        break;
    case HKXQuantType::QT_32bit:
    case HKXQuantType::QT_Uncompressed:
        br.Align(4);
        break;
    default:
        // QT_40bit: no special alignment needed beyond byte
        break;
    }

    // Read control point quaternions
    track.rotControlPoints.resize(numCP);
    for (uint16_t i = 0; i < numCP; ++i) {
        float q[4];
        readOneQuat(track.rotQuant, q);
        track.rotControlPoints[i] = {q[0], q[1], q[2], q[3]};
    }
}

} // anonymous namespace

bool ParseSplineBlock(const uint8_t* data, size_t dataSize,
                      int numTracks, int maskQuantSize, int /*maxFramesPerBlock*/,
                      std::vector<HKXSplineTrack>& outTracks) {
    BlockReader br(data, dataSize);
    outTracks.resize(numTracks);

    // Read transform masks (4 bytes per track)
    std::vector<HKXTransformMask> masks(numTracks);
    for (int t = 0; t < numTracks; ++t) {
        masks[t].quantizationTypes = br.ReadU8();
        masks[t].positionTypes = br.ReadU8();
        masks[t].rotationTypes = br.ReadU8();
        masks[t].scaleTypes = br.ReadU8();
    }

    // The mask+quantization region is exactly maskQuantSize bytes from the block start.
    // It includes transform masks (4 bytes each) plus float track masks.
    // Seek directly past the full region.
    br.SeekTo(static_cast<size_t>(maskQuantSize));
    br.Align(4);

    // Parse per-track data
    for (int t = 0; t < numTracks; ++t) {
        auto& track = outTracks[t];
        const auto& mask = masks[t];

        // Extract types
        for (int c = 0; c < 3; ++c) {
            track.posType[c] = mask.GetPositionComponentType(c);
            track.scaleType[c] = mask.GetScaleComponentType(c);
        }
        track.posQuant = mask.GetPositionQuantType();
        track.rotQuant = mask.GetRotationQuantType();
        track.scaleQuant = mask.GetScaleQuantType();
        track.rotType = mask.GetRotationTrackType();

        // Position
        ReadVectorTrack(br, track,
                        track.posType, track.posQuant, 0.0f,
                        track.posNumControlPoints, track.posDegree,
                        track.posKnots, track.posBBox,
                        track.posStatic, track.posControlPoints);

        br.Align(4);

        // Rotation
        ReadRotationTrack(br, track);

        br.Align(4);

        // Scale
        ReadVectorTrack(br, track,
                        track.scaleType, track.scaleQuant, 1.0f,
                        track.scaleNumControlPoints, track.scaleDegree,
                        track.scaleKnots, track.scaleBBox,
                        track.scaleStatic, track.scaleControlPoints);

        br.Align(4);
    }

    if (!br.IsValid()) {
        SKSE::log::warn("[SplineDecompressor] Block reader went out of bounds during parsing");
        return false;
    }

    return true;
}

} // namespace SkyrimRenderer
