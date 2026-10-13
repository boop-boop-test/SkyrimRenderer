// ── PostProcess/Bloom.ps.hlsl ────────────────────────────────────────────────
// Optional single-pass bloom for the post-process stack (stage 1 = Post):
//
//   pass = SkyrimRenderer_AddPostProcess(scene, "Bloom", 1,
//              "<shader dir>/PostProcess/Bloom.ps.hlsl", 100);
//   float p[3] = { threshold, intensity, radiusPixels };
//   SkyrimRenderer_SetPostProcessParams(scene, pass, p, 3);
//
// Params (b0, <= 0 picks the default): x = brightness threshold (0.75),
// y = intensity (0.6), z = radius in pixels (8). Bright parts of the image
// (glows, emissive maps, hot highlights) are gathered from two rings of taps
// around each pixel and added back. The input is the preview's sRGB-encoded,
// premultiplied target; bloom is computed in linear and written back the same
// way, raising alpha where it spills onto a transparent background.
// ─────────────────────────────────────────────────────────────────────────────

cbuffer Params : register(b0) {
    float4 params0;   // x = threshold, y = intensity, z = radius (pixels)
    float4 params1;
    float4 params2;
    float4 params3;
};

cbuffer Scene : register(b1) {
    float2 resolution;
    float2 texelSize;
    float  time;
    float  frameCount;
    float2 scenePad;
};

Texture2D    inputTexture : register(t0);
SamplerState inputSampler : register(s0);

float3 ToLinear(float3 c) { return pow(saturate(c), 2.2); }
float3 ToGamma(float3 c)  { return pow(saturate(c), 1.0 / 2.2); }

float3 BrightPass(float2 uv, float threshold) {
    float3 c = ToLinear(inputTexture.SampleLevel(inputSampler, uv, 0).rgb);
    float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
    // Soft knee: ramps in over [threshold * 0.5, threshold].
    float w = saturate((lum - threshold * 0.5) / max(threshold * 0.5, 1e-4));
    return c * w * w;
}

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET {
    float threshold = params0.x > 0.0 ? params0.x : 0.75;
    float intensity = params0.y > 0.0 ? params0.y : 0.6;
    float radius    = params0.z > 0.0 ? params0.z : 8.0;

    float4 src = inputTexture.SampleLevel(inputSampler, uv, 0);

    // 1 centre + 2 rings x 12 taps, Gaussian-weighted by ring distance.
    float3 sum = BrightPass(uv, threshold);
    float  wsum = 1.0;
    [unroll] for (int ring = 1; ring <= 2; ++ring) {
        float r = radius * ring * 0.5;
        float w = exp(-(ring * ring) * 0.5);
        [unroll] for (int i = 0; i < 12; ++i) {
            float a = (i + ring * 0.5) * (6.2831853 / 12.0);
            float2 offset = float2(cos(a), sin(a)) * r * texelSize;
            sum += BrightPass(uv + offset, threshold) * w;
            wsum += w;
        }
    }
    float3 bloom = sum / wsum * intensity;

    float3 color = ToGamma(ToLinear(src.rgb) + bloom);
    float glow = saturate(dot(bloom, float3(0.2126, 0.7152, 0.0722)));
    return float4(color, max(src.a, glow));
}
