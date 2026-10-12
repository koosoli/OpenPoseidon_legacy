// The ground as the editor shows it: the cell's texture (or a height / slope picture),
// sunlight, the sea's tint below 0, cell grid, contour lines and the brush.
sampler2D atlas : register(s0);
float4 sun : register(c0);      // xyz = towards the sun, w = ambient
float4 view : register(c1);     // x = 0 textures, 1 heights, 2 slopes; y = grid on; z = contour interval (0 = none); w = cell size
float4 brush : register(c2);    // xy = centre (x, z), z = radius, w = shown
float4 range : register(c3);    // x = lowest, y = highest height of the island; z = second ring radius (0 = none)
float4 brushTint : register(c4);

float3 HeightColour(float t)
{
    // green lowlands through yellow to brown and white tops
    float3 a = float3(0.20, 0.45, 0.18), b = float3(0.72, 0.68, 0.30), c = float3(0.50, 0.36, 0.24), d = float3(0.95, 0.95, 0.95);
    if (t < 0.35)
        return lerp(a, b, t / 0.35);
    if (t < 0.7)
        return lerp(b, c, (t - 0.35) / 0.35);
    return lerp(c, d, (t - 0.7) / 0.3);
}

float4 main(float2 uv : TEXCOORD0, float3 world : TEXCOORD1, float3 normal : TEXCOORD2) : COLOR0
{
    float3 n = normalize(normal);
    float3 base = tex2D(atlas, uv).rgb;
    if (view.x > 1.5)
    {
        float slope = saturate(1.0 - n.y) * 3.0; // 0 flat .. 1 at about 48 degrees
        base = lerp(float3(0.25, 0.6, 0.25), float3(0.9, 0.2, 0.15), saturate(slope));
    }
    else if (view.x > 0.5)
    {
        base = HeightColour(saturate((world.y - max(range.x, 0.0)) / max(range.y - max(range.x, 0.0), 1.0)));
    }
    float light = sun.w + (1.0 - sun.w) * saturate(dot(n, sun.xyz));
    float3 c = base * light;

    // under the sea: darker and bluer with depth
    if (world.y < 0.0)
    {
        float depth = saturate(-world.y / 30.0);
        c = lerp(c, float3(0.05, 0.16, 0.26), 0.35 + 0.55 * depth);
    }

    float2 cellPos = world.xz / view.w;
    float2 dCell = fwidth(cellPos);
    if (view.y > 0.5)
    {
        float2 g = abs(frac(cellPos - 0.5) - 0.5) / max(dCell, 1e-5);
        float lineK = 1.0 - saturate(min(g.x, g.y));
        // fade out where cells get smaller than a few pixels
        lineK *= saturate(1.0 - max(dCell.x, dCell.y) * 4.0);
        c = lerp(c, float3(0.0, 0.0, 0.0), lineK * 0.45);
    }
    if (view.z > 0.0)
    {
        float h = world.y / view.z;
        float lineK = 1.0 - saturate(abs(frac(h - 0.5) - 0.5) / max(fwidth(h), 1e-5));
        c = lerp(c, float3(0.15, 0.1, 0.05), lineK * 0.5);
    }
    if (brush.w > 0.5)
    {
        float d = length(world.xz - brush.xy);
        float w = max(fwidth(d) * 1.5, 0.01);
        float ring = 1.0 - saturate(abs(d - brush.z) / w);
        float inner = d < brush.z ? 0.12 : 0.0;
        float centre = 1.0 - saturate(d / (w * 2.0));
        float ring2 = range.z > 0.0 ? (1.0 - saturate(abs(d - range.z) / w)) * 0.6 : 0.0;
        c = lerp(c, brushTint.rgb, saturate(max(max(ring, ring2), max(inner, centre))) * brushTint.a);
    }
    return float4(c, 1.0);
}
