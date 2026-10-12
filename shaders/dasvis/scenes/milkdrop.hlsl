// MilkDrop: one of projectM's presets as a DasVis scene (turned on in the menu). The app renders the preset and hands
// the picture over (t_sim, rows bottom-up as OpenGL reads them); DasVis goes over it: a punch on the kick, the colours
// pulled a little towards the song's palette, then DasVis's bloom, transitions, drops and overlays. Until the first
// picture arrives (MilkDrop takes about a second to start), the last frame holds.
#define DASVIS_SCENE
#include "dasvis.hlsli"

float3 scene(float2 uv, float2 p) {
    float3 c = t_src.SampleLevel(s_clamp, uv, 0).rgb * 0.98;
    if (u_sim.x > 0.5) {
        float2 q = 0.5 + (uv - 0.5) / (1 + 0.035 * KICK * INTENSITY);
        c = pow(saturate(t_sim.SampleLevel(s_clamp, float2(q.x, 1 - q.y), 0).rgb), 2.2);  // its bytes are sRGB: to linear light
        float l = dot(c, float3(0.2126, 0.7152, 0.0722));
        c = lerp(c, PAL(saturate(l * 1.5)) * l * 1.6, 0.2) * (1.1 + 0.4 * KICK * FLASH);
    }
    return c;
}
