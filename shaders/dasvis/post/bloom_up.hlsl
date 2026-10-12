// Bloom, going up: the smaller level, tent-filtered with eight taps, added onto the next larger one (additive blend).
#include "dasvis.hlsli"

float4 main(VsOut v) : SV_Target {
    float2 h = u_pass.xy * 0.5;
    float3 s = t_src.SampleLevel(s_clamp, v.uv + float2(-h.x * 2, 0), 0).rgb;
    s += t_src.SampleLevel(s_clamp, v.uv + float2(h.x * 2, 0), 0).rgb;
    s += t_src.SampleLevel(s_clamp, v.uv + float2(0, -h.y * 2), 0).rgb;
    s += t_src.SampleLevel(s_clamp, v.uv + float2(0, h.y * 2), 0).rgb;
    s += t_src.SampleLevel(s_clamp, v.uv + float2(-h.x, h.y), 0).rgb * 2;
    s += t_src.SampleLevel(s_clamp, v.uv + float2(h.x, h.y), 0).rgb * 2;
    s += t_src.SampleLevel(s_clamp, v.uv + float2(-h.x, -h.y), 0).rgb * 2;
    s += t_src.SampleLevel(s_clamp, v.uv + float2(h.x, -h.y), 0).rgb * 2;
    return float4(s / 12, 1);
}
