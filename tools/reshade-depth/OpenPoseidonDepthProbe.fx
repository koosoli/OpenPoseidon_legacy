// SPDX-License-Identifier: GPL-3.0-or-later
// Original diagnostic only. This is not AO, GI, reflections or ray tracing.
#ifndef OPENPOSEIDON_ENGINE_DEPTH
#define OPENPOSEIDON_ENGINE_DEPTH 0
#endif
texture2D ProbeColor : COLOR;
#if OPENPOSEIDON_ENGINE_DEPTH
texture2D ProbeDepth : OP_SCENE_DEPTH;
#else
texture2D ProbeDepth : DEPTH;
#endif
sampler2D ProbeColorSampler { Texture = ProbeColor; };
sampler2D ProbeDepthSampler { Texture = ProbeDepth; };
#if OPENPOSEIDON_ENGINE_DEPTH
// Versioned engine bridge drives this only after matching native device/queue,
// resource identity, dimensions and a completed nonuniform numeric witness.
uniform bool DepthReady = false;
#else
uniform bool DepthReady < source = "bufready_depth"; > = false;
#endif
uniform bool FlipY < ui_label = "Flip depth vertically"; > = false;
uniform bool ReversedZ < ui_label = "Reversed depth (near=1, far=0)"; > = true;
uniform float DepthPower < ui_label = "Depth visualization power"; ui_type = "slider"; ui_min = 0.02; ui_max = 1.0; > = 0.125;
uniform float Overlay < ui_label = "Scene overlay strength"; ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; > = 0.25;

void ProbeVS(uint id : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD)
{
    uv = float2(id == 2 ? 2.0 : 0.0, id == 1 ? 2.0 : 0.0);
    position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float4 ProbePS(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float2 depthUV = float2(uv.x, FlipY ? 1.0 - uv.y : uv.y);
    float raw = saturate(tex2D(ProbeDepthSampler, depthUV).r);
    float nearSignal = ReversedZ ? raw : 1.0 - raw;
    // Power expands small infinite/reversed-Z values without claiming metric
    // linearization. Empty or uniformly filled depth stays visibly uniform.
    float gray = 1.0 - pow(nearSignal, DepthPower);
    float3 scene = tex2D(ProbeColorSampler, uv).rgb;
    // Generic mode: selected resource only. Engine mode: versioned binding and
    // numeric witness. Neither mode's green strip proves visual alignment.
    if (uv.y < 0.02)
        return DepthReady ? float4(0.0, 0.7, 0.0, 1.0) : float4(0.8, 0.0, 0.0, 1.0);
    return float4(lerp(gray.xxx, scene, Overlay), 1.0);
}
technique OpenPoseidonDepthProbe < ui_label = "OpenPoseidon depth availability probe (diagnostic)"; >
{
    pass { VertexShader = ProbeVS; PixelShader = ProbePS; }
}
