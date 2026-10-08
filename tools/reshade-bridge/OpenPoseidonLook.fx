// SPDX-License-Identifier: GPL-3.0-or-later
// Original, small color-only comparison effect. No game assets or depth needed.
texture2D SceneColor : COLOR;
sampler2D SceneSampler { Texture = SceneColor; };
uniform float Strength < ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; > = 0.45;

void FullscreenVS(uint id : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD)
{
    uv = float2(id == 2 ? 2.0 : 0.0, id == 1 ? 2.0 : 0.0);
    position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float4 LookPS(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float3 color = tex2D(SceneSampler, uv).rgb;
    float2 pixel = float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT);
    float3 neighborhood = (tex2D(SceneSampler, uv + float2(pixel.x, 0.0)).rgb
                         + tex2D(SceneSampler, uv - float2(pixel.x, 0.0)).rgb
                         + tex2D(SceneSampler, uv + float2(0.0, pixel.y)).rgb
                         + tex2D(SceneSampler, uv - float2(0.0, pixel.y)).rgb) * 0.25;
    float3 sharpened = color + clamp(color - neighborhood, -0.045, 0.045) * 0.65;
    float luminance = dot(sharpened, float3(0.2126, 0.7152, 0.0722));
    float3 graded = lerp(luminance.xxx, sharpened, 0.96);
    graded = (graded - 0.5) * 1.06 + 0.5;
    return float4(saturate(lerp(color, graded, Strength)), 1.0);
}
technique OpenPoseidonLook
{
    pass { VertexShader = FullscreenVS; PixelShader = LookPS; }
}
