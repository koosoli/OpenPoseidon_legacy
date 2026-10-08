// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OpenPoseidon contributors
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
// of the Software, and to permit persons to whom the Software is furnished to do
// so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
// Original adapter for the engine's forward-infinite projection + reversed Z.
// DH retains normalized depth internally; view positions below are in metres.
texture2D OpSceneDepth : OP_SCENE_DEPTH;
sampler2D OpSceneDepthSampler { Texture = OpSceneDepth; MinFilter = POINT; MagFilter = POINT; };
uniform bool OpSceneReady = false;
// Separate from depth: the producer must explicitly prove its native surface
// fog is disabled. Fogged post-tonemap color cannot be treated as radiance.
// Missing/older producer or add-on leaves beauty unchanged, debug available.
uniform bool OpSceneGiReady = false;
// Driven every effects frame by the checked bridge, never preset estimates.
uniform float4 OpSceneProjection = float4(1.0, 1.0, 0.1, 0.0); // p00,p55,near,unused
uniform float2 OpSceneOffset = float2(0.0, 0.0); // projection p08,p09
// Explicit DH working range, not the engine's infinite far plane.
uniform float OpSceneRange < ui_label = "Scene depth working range (metres)";
    ui_type = "slider"; ui_min = 100.0; ui_max = 10000.0; > = 1000.0;

float OpSceneLinearDepth(float2 uv)
{
    float raw = tex2Dlod(OpSceneDepthSampler, float4(uv, 0, 0)).r;
    // Sky/invalid binding is a sentinel beyond the bounded DH tracing range.
    return OpSceneReady && raw > 0.0 && raw <= 1.0 && OpSceneRange > 0.0 ? OpSceneProjection.z / (raw * OpSceneRange) : 2.0;
}
// A bounded screen-space trace has no receiver/donor outside this open range.
// Native fog/sky colors are not radiance samples of a visible valid surface.
bool OpSceneTraceDepthValid(float normalizedDepth)
{
    return OpSceneReady && normalizedDepth > 0.0 && normalizedDepth < 1.0;
}
// The pinned DH near-depth filter reaches zero at 10 m in our 1000 m
// normalization. Retain that bandwidth in metres everywhere: its original
// (1 - 2*depth) factor disabled rejection entirely at/after 500 m. Symmetric,
// independent of working range, with no minimum weight leaking other layers.
float OpSceneDepthFilterWeight(float centerDepth, float sampleDepth)
{
    return OpSceneReady && centerDepth > 0.0 && centerDepth < 1.0 && sampleDepth > 0.0 && sampleDepth < 1.0 ?
        saturate(1.0 - abs(sampleDepth - centerDepth) * OpSceneRange * 0.1) : 0.0;
}
float3 OpSceneViewPosition(float2 uv, float normalizedDepth)
{
    float z = normalizedDepth * OpSceneRange;
    // DH uses screen-down Y. This orthonormal reflection preserves distances,
    // angles and its cross-product convention, with the inverse below exact.
    return float3((uv.x * 2.0 - 1.0 - OpSceneOffset.x) * z / OpSceneProjection.x,
                  (uv.y * 2.0 - 1.0 + OpSceneOffset.y) * z / OpSceneProjection.y, z);
}
float3 OpSceneScreenPosition(float3 position)
{
    float2 projected = float2(position.x * OpSceneProjection.x,
                             position.y * OpSceneProjection.y) / position.z;
    return float3((projected + float2(OpSceneOffset.x, -OpSceneOffset.y) + 1.0) * 0.5,
                  position.z / OpSceneRange);
}
