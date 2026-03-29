#pragma once

/// @file SplineDecompressor.h
/// @brief B-spline evaluation and quaternion decompression for Havok spline-compressed animations.
///
/// Implements the mathematical core of hkaSplineCompressedAnimation decompression:
/// - Uniform B-spline basis function evaluation (Cox-de Boor)
/// - Knot span binary search
/// - Quantized quaternion decompression (32-bit, 40-bit, 48-bit formats)
/// - Per-track sampling at arbitrary time values
///
/// Format reference: HKX2Library (MIT, ret2end), HavokLib (PredatorCZ).

#include "SkyrimRenderer/HKXParser.h"

#include <cstdint>

namespace SkyrimRenderer {

// ── Quaternion Decompression ────────────────────────────────────────────────

/// Decompress a 32-bit spherical-coordinate quaternion.
/// @param packed  4 bytes packed as uint32.
/// @param out     Receives xyzw quaternion.
void DecompressQuat32(uint32_t packed, float out[4]);

/// Decompress a 40-bit dropped-component quaternion (3 x 11-bit + flags).
/// @param bytes   5 bytes of packed data.
/// @param out     Receives xyzw quaternion.
void DecompressQuat40(const uint8_t bytes[5], float out[4]);

/// Decompress a 48-bit dropped-component quaternion (3 x int16 + flags).
/// @param bytes   6 bytes of packed data.
/// @param out     Receives xyzw quaternion.
void DecompressQuat48(const uint8_t bytes[6], float out[4]);

// ── B-Spline Evaluation ─────────────────────────────────────────────────────

/// Find the knot span containing the given parameter value.
/// @param degree          B-spline degree (typically 3).
/// @param frame           Parameter value (local frame within block).
/// @param controlPointCount  Number of control points.
/// @param knots           Knot vector (size = controlPointCount + degree + 1).
/// @return Knot span index.
int FindKnotSpan(int degree, float frame, int controlPointCount, const uint8_t* knots);

/// Evaluate a B-spline at a given parameter value for a scalar track.
/// @param degree          B-spline degree.
/// @param frame           Parameter value.
/// @param controlPointCount  Number of control points.
/// @param knots           Knot vector.
/// @param controlPoints   Scalar control point values.
/// @return Interpolated scalar value.
float EvaluateScalarSpline(int degree, float frame, int controlPointCount,
                           const uint8_t* knots, const float* controlPoints);

/// Evaluate a B-spline at a given parameter value for a quaternion track.
/// @param degree          B-spline degree.
/// @param frame           Parameter value.
/// @param controlPointCount  Number of control points.
/// @param knots           Knot vector.
/// @param controlPoints   Quaternion control points (xyzw each).
/// @param out             Receives interpolated + normalized quaternion.
void EvaluateQuaternionSpline(int degree, float frame, int controlPointCount,
                              const uint8_t* knots,
                              const HKXSplineTrack::Quat* controlPoints,
                              float out[4]);

// ── High-Level Track Sampling ───────────────────────────────────────────────

/// Sample a single track's transform at a given local frame within a block.
/// @param track   The decompressed spline track data.
/// @param frame   Local frame value within the block.
/// @param outPos  Receives xyz position.
/// @param outRot  Receives xyzw rotation quaternion.
/// @param outScale Receives xyz scale.
void SampleTrack(const HKXSplineTrack& track, float frame,
                 float outPos[3], float outRot[4], float outScale[3]);

// ── Block Parsing ───────────────────────────────────────────────────────────

/// Parse a single compressed data block into per-track spline data.
/// @param data            Pointer to block start within the compressed blob.
/// @param dataSize        Remaining bytes from block start.
/// @param numTracks       Number of transform tracks.
/// @param maskQuantSize   Size of the mask+quantization header.
/// @param maxFramesPerBlock Max frames in a block.
/// @param outTracks       Receives parsed track data (resized to numTracks).
/// @return true on success.
bool ParseSplineBlock(const uint8_t* data, size_t dataSize,
                      int numTracks, int maskQuantSize, int maxFramesPerBlock,
                      std::vector<HKXSplineTrack>& outTracks);

} // namespace SkyrimRenderer
