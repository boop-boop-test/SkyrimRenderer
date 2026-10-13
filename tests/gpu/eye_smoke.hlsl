// Smoke test only (NTJH supplies the real eye_remap.hlsl): radial remap of
// the pupil and iris around Center, always sampling the original.
cbuffer Params {
    float2 Center = float2(0.5, 0.5);
    float IrisRadius = 0.15;
    float PupilRadius = 0.05;
    float PupilScale = 1.0;
    float IrisScale = 1.0;
    float4 Tint = float4(1, 1, 1, 1);
};
cbuffer GpuJob { uint2 OutputSize; uint MipLevel; uint OutputSRGB; };
Texture2D Source;
RWTexture2D<float4> Result;
SamplerState LinearClamp : register(s0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= OutputSize)) return;
    float2 uv = (id.xy + 0.5) / OutputSize;
    float2 d = uv - Center;
    float r = length(d);
    float outer = IrisRadius * 1.3;           // untouched beyond this
    float p0 = PupilRadius, i0 = IrisRadius;  // source radii
    float p1 = PupilRadius * PupilScale, i1 = IrisRadius * IrisScale;
    p1 = min(p1, i1 * 0.9);
    float src = r;
    if (r < p1)          src = r * p0 / max(p1, 1e-5);
    else if (r < i1)     src = p0 + (r - p1) * (i0 - p0) / max(i1 - p1, 1e-5);
    else if (r < outer)  src = i0 + (r - i1) * (outer - i0) / max(outer - i1, 1e-5);
    float2 suv = Center + (r > 1e-6 ? d / r * src : 0);
    float4 c = Source.SampleLevel(LinearClamp, suv, 0);
    if (r < i1) c.rgb *= Tint.rgb;
    Result[id.xy] = c;
}
