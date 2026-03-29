#include "SkyrimRenderer/AnimationTypes.h"

namespace SkyrimRenderer {

void BoneTransform::TransformPoint(const float* in, float* out) const {
    // Apply scale, then rotation, then translation.
    float scaled[3] = {in[0] * scale, in[1] * scale, in[2] * scale};
    rotation.TransformVector(scaled, out);
    out[0] += translation[0];
    out[1] += translation[1];
    out[2] += translation[2];
}

void BoneTransform::TransformDirection(const float* in, float* out) const {
    // Rotation only, no scale or translation.
    rotation.TransformVector(in, out);
}

BoneTransform BoneTransform::Compose(const BoneTransform& other) const {
    // result = this * other
    // Applies 'other' first, then 'this'.
    //
    // For a point p:
    //   other(p)  = otherRot * (p * otherScale) + otherTrans
    //   this(q)   = thisRot * (q * thisScale) + thisTrans
    //   result(p) = this(other(p))
    //
    // Combined rotation:  thisRot * otherRot
    // Combined scale:     thisScale * otherScale
    // Combined translate: thisRot * (otherTrans * thisScale) + thisTrans

    BoneTransform result;
    result.rotation = rotation * other.rotation;
    result.scale = scale * other.scale;

    float scaledOtherTrans[3] = {
        other.translation[0] * scale,
        other.translation[1] * scale,
        other.translation[2] * scale
    };
    float rotatedTrans[3];
    rotation.TransformVector(scaledOtherTrans, rotatedTrans);

    result.translation[0] = rotatedTrans[0] + translation[0];
    result.translation[1] = rotatedTrans[1] + translation[1];
    result.translation[2] = rotatedTrans[2] + translation[2];

    return result;
}

BoneTransform BoneTransform::Inverse() const {
    BoneTransform result;
    float invScale = (scale != 0.0f) ? (1.0f / scale) : 0.0f;
    result.rotation = rotation.Transpose();
    result.scale = invScale;
    // inverse translation: -(1/s)*R^T*t
    float negT[3] = {-translation[0], -translation[1], -translation[2]};
    float scaledNegT[3] = {negT[0] * invScale, negT[1] * invScale, negT[2] * invScale};
    result.rotation.TransformVector(scaledNegT, result.translation);
    return result;
}

BoneTransform BoneTransform::Lerp(const BoneTransform& a, const BoneTransform& b, float t) {
    BoneTransform result;
    result.rotation = Mat3::Lerp(a.rotation, b.rotation, t);
    result.scale = a.scale * (1.0f - t) + b.scale * t;
    result.translation[0] = a.translation[0] * (1.0f - t) + b.translation[0] * t;
    result.translation[1] = a.translation[1] * (1.0f - t) + b.translation[1] * t;
    result.translation[2] = a.translation[2] * (1.0f - t) + b.translation[2] * t;
    return result;
}

} // namespace SkyrimRenderer
