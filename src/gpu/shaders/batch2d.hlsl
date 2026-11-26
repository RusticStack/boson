// Boson gpu::Renderer2D batch shader. Compiled at build time with fxc (vs_4_0 / ps_4_0, feature level 10_0+)
// into generated headers; see CMakeLists.txt (boson_gpu) and docs/GPU.md.
//
// One instance = one quad: rect (x, y, w, h in target pixels), uv rect, straight RGBA color and params
// (x = corner radius, y = kind, z = border width). The 4 strip vertices come from SV_VertexID.
// kind: 0 solid, 1 rounded rect (SDF, 1px anti-aliasing; border > 0 = outline), 2 glyph (R8 atlas coverage),
// 3 image (RGBA texture, straight alpha). Output is premultiplied alpha (blend ONE / INV_SRC_ALPHA).

cbuffer Globals : register(b0)
{
    float2 g_inv_half_size; // 2 / width, 2 / height
    float2 g_pad;
};

struct Instance
{
    float4 rect : RECT;
    float4 uv : UV;
    float4 color : COLOR;
    float4 params : PARAMS;
    uint vertex_id : SV_VertexID;
};

struct VsOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float2 local : TEXCOORD1;
    float4 color : COLOR0;
    nointerpolation float4 params : TEXCOORD2;
    nointerpolation float2 half_size : TEXCOORD3;
};

VsOut vs_main(Instance input)
{
    const float2 corner = float2(float(input.vertex_id & 1u), float(input.vertex_id >> 1));
    const float2 p = input.rect.xy + corner * input.rect.zw;
    VsOut output;
    output.position = float4(p.x * g_inv_half_size.x - 1.0, 1.0 - p.y * g_inv_half_size.y, 0.0, 1.0);
    output.uv = lerp(input.uv.xy, input.uv.zw, corner);
    output.local = (corner - 0.5) * input.rect.zw;
    output.color = input.color;
    output.params = input.params;
    output.half_size = input.rect.zw * 0.5;
    return output;
}

Texture2D g_atlas : register(t0);
Texture2D g_image : register(t1);
SamplerState g_point : register(s0);
SamplerState g_linear : register(s1);

float4 ps_main(VsOut input) : SV_Target
{
    const float kind = input.params.y;
    float alpha = input.color.a;
    if (kind > 2.5)
    {
        const float4 texel = g_image.SampleLevel(g_linear, input.uv, 0.0);
        const float a = texel.a * alpha;
        return float4(texel.rgb * input.color.rgb * a, a);
    }
    if (kind > 1.5)
    {
        alpha *= g_atlas.SampleLevel(g_point, input.uv, 0.0).r;
    }
    else if (kind > 0.5)
    {
        const float radius = min(input.params.x, min(input.half_size.x, input.half_size.y));
        const float2 q = abs(input.local) - input.half_size + radius;
        const float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
        float coverage = saturate(0.5 - d);
        if (input.params.z > 0.0)
        {
            coverage -= saturate(0.5 - (d + input.params.z));
        }
        alpha *= coverage;
    }
    return float4(input.color.rgb * alpha, alpha);
}
