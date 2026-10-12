// An object's model: its texture, cut out where it is clear, in the same sunlight as the ground.
sampler2D tex : register(s0);
float4 sun : register(c0); // xyz = towards the sun, w = ambient

float4 main(float2 uv : TEXCOORD0, float3 world : TEXCOORD1, float3 normal : TEXCOORD2) : COLOR0
{
    float4 t = tex2D(tex, uv);
    clip(t.a - 0.4);
    float3 n = normalize(normal);
    // leaves and thin walls are seen from both sides: the light wraps round them
    float light = sun.w + (1.0 - sun.w) * saturate(dot(n, sun.xyz) * 0.6 + 0.4);
    float3 c = t.rgb * light;
    if (world.y < 0.0)
    {
        float depth = saturate(-world.y / 30.0);
        c = lerp(c, float3(0.05, 0.16, 0.26), 0.35 + 0.55 * depth);
    }
    return float4(c, 1.0);
}
