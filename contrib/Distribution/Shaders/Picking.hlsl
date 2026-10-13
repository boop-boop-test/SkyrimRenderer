// ── Picking.hlsl ─────────────────────────────────────────────────────────────
// Mesh-ID pass (picking) and the highlight overlay built from it.
//
//   PSPickID     paired with the preview vertex shaders (Preview.vs.hlsl);
//                writes (meshIndex + 1) | (highlightSlot << 24) to R32_UINT.
//   VSFullscreen + PSHighlight   fullscreen triangle that outlines / tints the
//                highlight slots of the last ID pass onto the scene.
// ─────────────────────────────────────────────────────────────────────────────

#include "Common.hlsl"

#define HIGHLIGHT_OUTLINE 1u
#define HIGHLIGHT_TINT    2u
#define MAX_HIGHLIGHTS    16
#define MAX_OUTLINE       4

// ── ID pass ─────────────────────────────────────────────────────────────────

cbuffer PickParams : register(b4) {
    uint  pickId;            // value written for this mesh
    float pickAlphaCutoff;   // < 0: no alpha test
    uint  pickHasDiffuse;
    uint  pickPad;
};

uint PSPickID(PSInput input) : SV_Target {
    if (pickAlphaCutoff >= 0.0 && pickHasDiffuse != 0) {
        // Same test as the lit draw (Preview.ps.hlsl): texture alpha only.
        clip(diffuseTexture.Sample(diffuseSampler, input.uv).a - pickAlphaCutoff);
    }
    return pickId;
}

// ── Highlight overlay ───────────────────────────────────────────────────────

Texture2D<uint> pickIds : register(t10);

cbuffer HighlightParams : register(b5) {
    float4 hlColor[MAX_HIGHLIGHTS];   // rgb + opacity
    uint4  hlParams[MAX_HIGHLIGHTS];  // x = mode bits, y = outline width (px)
};

float4 VSFullscreen(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

uint SlotAt(int2 p) {
    // Out-of-range loads return 0 (background).
    return pickIds.Load(int3(p, 0)) >> 24;
}

// Premultiplied output (blend ONE, INV_SRC_ALPHA): keeps texture-mode targets
// premultiplied and equals ordinary alpha blending over opaque pixels.
float4 PSHighlight(float4 pos : SV_Position) : SV_Target {
    int2 p = int2(pos.xy);
    uint self = SlotAt(p);
    float4 result = float4(0.0, 0.0, 0.0, 0.0);

    if (self != 0 && (hlParams[self - 1].x & HIGHLIGHT_TINT) != 0) {
        float4 c = hlColor[self - 1];
        result = float4(c.rgb * c.a, c.a);
    }

    // Outline: pixels outside a highlighted item, within its outline width.
    uint best = 0;
    float bestDist = 1e6;
    [loop] for (int dy = -MAX_OUTLINE; dy <= MAX_OUTLINE; ++dy) {
        [loop] for (int dx = -MAX_OUTLINE; dx <= MAX_OUTLINE; ++dx) {
            uint s = SlotAt(p + int2(dx, dy));
            if (s == 0 || s == self) continue;
            if ((hlParams[s - 1].x & HIGHLIGHT_OUTLINE) == 0) continue;
            float d = length(float2(dx, dy));
            if (d <= float(hlParams[s - 1].y) + 0.5 && d < bestDist) {
                bestDist = d;
                best = s;
            }
        }
    }
    if (best != 0) {
        float4 c = hlColor[best - 1];
        // Full inside the width, the last pixel softened.
        float edge = saturate(float(hlParams[best - 1].y) + 1.0 - bestDist);
        float a = c.a * edge;
        result = float4(c.rgb * a, a) + result * (1.0 - a);
    }

    if (result.a <= 0.0) discard;
    return result;
}
