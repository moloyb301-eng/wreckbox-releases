// The flash limiter's measure: the picture's relative luminance as it will look on screen (exposure and tone mapping
// applied), from the smallest bloom level (already a blurred average) into a 16 × 16 target read back by the CPU.
#include "dasvis.hlsli"

float4 main(VsOut v) : SV_Target {
    float3 c = aces(t_src.SampleLevel(s_clamp, v.uv, 0).rgb * EXPOSURE);
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}
