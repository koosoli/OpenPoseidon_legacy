// Coloured lines and flat shapes: object markers, the sea, the selection.
float4x4 viewProj : register(c0);

struct VSOut
{
    float4 color : COLOR0;
    float4 pos : POSITION;
};

VSOut main(float3 pos : POSITION, float4 color : COLOR0)
{
    VSOut o;
    o.pos = mul(viewProj, float4(pos, 1.0));
    o.color = color;
    return o;
}
