// Bloom, going down: half the size each pass, five bilinear taps (the dual-filter blur). No threshold: the bloom is
// mixed in, not added, so it softens highlights without washing out the picture.
#include "dasvis.hlsli"

float4 main(VsOut v) : SV_Target {
    float2 h = u_pass.xy * 0.5;  // half a texel of the source
    float3 s = t_src.SampleLevel(s_clamp, v.uv, 0).rgb * 4;
    s += t_src.SampleLevel(s_clamp, v.uv - h, 0).rgb;
    s += t_src.SampleLevel(s_clamp, v.uv + h, 0).rgb;
    s += t_src.SampleLevel(s_clamp, v.uv + float2(h.x, -h.y), 0).rgb;
    s += t_src.SampleLevel(s_clamp, v.uv - float2(h.x, -h.y), 0).rgb;
    return float4(s / 8, 1);
}
