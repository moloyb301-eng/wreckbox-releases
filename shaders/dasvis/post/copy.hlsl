// A plain resample: carries the feedback picture over when the render size changes (no black frame, which would
// itself be a flash).
#include "dasvis.hlsli"

float4 main(VsOut v) : SV_Target { return float4(t_src.SampleLevel(s_clamp, v.uv, 0).rgb, 1); }
