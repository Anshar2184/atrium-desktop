// Exact comparison of the current backdrop ROI with the ROI that was last
// presented. Out[0] = changed pixel count, Out[1] = max channel difference
// (0..255), Out[2 + tile] = per-tile max difference.

Texture2D<float4> Cur : register(t0);
Texture2D<float4> Prev : register(t1);
RWStructuredBuffer<uint> Out : register(u0);

cbuffer CompareParams : register(b0)
{
    uint Width;
    uint Height;
    uint TilesX;
    uint TileSize;
};

[numthreads(16, 16, 1)]
void CS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;
    float3 d = abs(Cur.Load(int3(id.xy, 0)).rgb - Prev.Load(int3(id.xy, 0)).rgb);
    uint m = (uint)round(max(d.r, max(d.g, d.b)) * 255.0);
    if (m == 0)
        return;
    InterlockedAdd(Out[0], 1);
    InterlockedMax(Out[1], m);
    InterlockedMax(Out[2 + (id.y / TileSize) * TilesX + id.x / TileSize], m);
}
