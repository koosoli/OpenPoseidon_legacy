// The ground: position, normal and the place in the texture atlas of each cell corner.
float4x4 viewProj : register(c0);

struct VSOut
{
    float2 uv : TEXCOORD0;
    float3 world : TEXCOORD1;
    float3 normal : TEXCOORD2;
    float4 pos : POSITION;
};

VSOut main(float3 pos : POSITION, float3 normal : NORMAL, float2 uv : TEXCOORD0)
{
    VSOut o;
    o.pos = mul(viewProj, float4(pos, 1.0));
    o.uv = uv;
    o.world = pos;
    o.normal = normal;
    return o;
}
