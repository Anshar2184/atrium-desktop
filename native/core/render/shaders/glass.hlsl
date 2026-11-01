// Atrium glass material. One signed-distance shape; Liquid and Frosted are
// parameter presets (docs/RENDERING-DESIGN.md). Ported from poc/Probe/Shaders.

cbuffer GlassParams : register(b0)
{
    float2 Size;        // window size in px
    float2 RoiOrigin;   // window origin inside the background ROI texture
    float2 RoiSize;     // ROI texture size
    float  Radius;      // corner radius in px
    float  Edge;        // width of the refracting rim in px
    float  Refract;     // max sample displacement in px
    float  Dispersion;  // relative channel spread of the displacement
    float  BlurLod;     // mip level used for the blurred backdrop
    float  TintMix;
    float4 Tint;
    float  Highlight;
    float  Shadow;
    float2 LightDir;    // unit vector pointing towards the light
    float  Inset;       // shadow room around the shape (sets the shadow's softness)
    float  BlurMix;     // 0 = sharp backdrop, 1 = fully blurred
    float2 _pad;
    float4 Shape;       // the glass shape in window px: left, top, right, bottom
    float4 Ripple;      // press ripple: centre x, y (window px), ring radius, strength (0 = none)
};

Texture2D Bg : register(t0);
SamplerState Linear : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut VS(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float sdRoundRect(float2 p, float2 b, float r)
{
    float2 q = abs(p) - b + r;
    return length(max(q, 0)) + min(max(q.x, q.y), 0) - r;
}

float3 bg(float2 px, float lod)
{
    return Bg.SampleLevel(Linear, (px + RoiOrigin) / RoiSize, lod).rgb;
}

float3 blurred(float2 px)
{
    // A few taps at a coarse mip hide the mip's box filter structure.
    float s = exp2(BlurLod) * 0.75;
    float3 c = bg(px, BlurLod) * 0.4;
    c += bg(px + float2(s, 0), BlurLod) * 0.15;
    c += bg(px - float2(s, 0), BlurLod) * 0.15;
    c += bg(px + float2(0, s), BlurLod) * 0.15;
    c += bg(px - float2(0, s), BlurLod) * 0.15;
    return c;
}

float4 PS(VSOut i) : SV_Target
{
    float2 px = i.uv * Size;
    float2 c = px - (Shape.xy + Shape.zw) * 0.5;
    float2 b = max((Shape.zw - Shape.xy) * 0.5, 1);
    float r = min(Radius, min(b.x, b.y));
    float d = sdRoundRect(c, b, r);

    // Soft drop shadow outside the shape.
    float ds = sdRoundRect(c - float2(0, Inset * 0.25), b, r);
    float shadowA = Shadow * exp(-max(ds, 0) / max(Inset * 0.35, 1)) * step(0, d);

    float aa = max(fwidth(d), 1e-3);
    float cov = saturate(0.5 - d / aa);
    if (cov <= 0)
        return float4(0, 0, 0, shadowA);

    float e = 1.0;
    float2 g = float2(sdRoundRect(c + float2(e, 0), b, r) - sdRoundRect(c - float2(e, 0), b, r),
                      sdRoundRect(c + float2(0, e), b, r) - sdRoundRect(c - float2(0, e), b, r));
    float gl = length(g);
    float2 n = gl > 1e-5 ? g / gl : float2(0, 0);

    float depth = max(-d, 0);
    float t = saturate(1 - depth / max(Edge, 1));
    float profile = 1 - sqrt(saturate(1 - t * t)); // circular lens cross-section
    float2 off = -n * Refract * profile;

    // Press ripple: a ring of displacement running outwards from the point that
    // was pressed, as if the glass gave way under the finger and sprang back.
    float ring = 0;
    if (Ripple.w > 0) {
        float2 dv = px - Ripple.xy;
        float dl = length(dv);
        float w = max(Ripple.z * 0.30, 6);
        float x = (dl - Ripple.z) / w;
        ring = exp(-x * x) * Ripple.w;
        float2 dir = dl > 1e-3 ? dv / dl : float2(0, 0);
        off += dir * ring * (6 + Refract * 0.35);
        off -= dv * 0.06 * Ripple.w * saturate(1 - dl / max(Ripple.z, 1)); // the dent inside the ring
    }

    float3 sharp;
    sharp.r = bg(px + off * (1 + Dispersion), 0).r;
    sharp.g = bg(px + off, 0).g;
    sharp.b = bg(px + off * (1 - Dispersion), 0).b;
    float3 soft = blurred(px + off);
    float3 col = lerp(sharp, soft, BlurMix);

    // Vibrancy: content under the glass reads a little richer and brighter,
    // as on macOS, before the tint is mixed in.
    float luma = dot(col, float3(0.2126, 0.7152, 0.0722));
    col = lerp(luma.xxx, col, 1.0 + 0.22 * Highlight) * (1.0 + 0.04 * Highlight);

    col = lerp(col, Tint.rgb, TintMix);

    // Light: a crisp specular line along the rim, strongest facing the light,
    // a softer glow just inside it, and a faint sheen from the top.
    float lit = saturate(dot(n, LightDir));
    float back = saturate(dot(n, -LightDir));
    float rimLine = 1 - smoothstep(0.0, 1.6, depth);
    float glow = saturate(1 - depth / (Edge * 1.6));
    col += Highlight * rimLine * (0.22 + 0.78 * lit + 0.35 * back);
    col += Highlight * glow * glow * lit * 0.16;
    float sheen = saturate(1 - (px.y - Shape.y) / max(Shape.w - Shape.y, 1));
    col += Highlight * 0.045 * sheen * sheen;

    // Depth: a very light shade in the band where the lens bends the most.
    float band = smoothstep(0.0, Edge * 0.5, depth) * (1 - smoothstep(Edge * 0.5, Edge * 1.3, depth));
    col *= 1 - 0.05 * band * (1 - lit);
    col += ring * 0.10 * (0.4 + Highlight);

    float4 glass = float4(saturate(col) * cov, cov);
    return glass + (1 - cov) * float4(0, 0, 0, shadowA);
}
