// Only the off-monitor ROI strips are shaded. EdgeSource contains the four
// current source borders, copied on the GPU; no captured frame is retained.
cbuffer EdgeParams : register(b0)
{
    int2 Origin;
    int2 Extent;
};
Texture2D EdgeSource : register(t0);

float4 PS(float4 position : SV_Position) : SV_Target
{
    int2 pixel = clamp(int2(position.xy) + Origin, int2(0, 0), Extent - 1);
    return EdgeSource.Load(int3(pixel, 0));
}
