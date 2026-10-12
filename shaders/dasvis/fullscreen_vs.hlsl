// One triangle that covers the target; uv 0–1 from the top left. Every DasVis pass uses it.
#include "dasvis.hlsli"

VsOut main(uint id : SV_VertexID) {
    VsOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
