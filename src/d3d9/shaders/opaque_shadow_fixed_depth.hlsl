float4 WorldViewProjection[4] : register(c0);
float ShadowBufferZBias : register(c4);
float4 ColorScale : register(c5);

void main(float4 worldPosition : TEXCOORD1,
          out float4 color : COLOR0)
{
    float depth = dot(worldPosition, float4(
        WorldViewProjection[0].z,
        WorldViewProjection[1].z,
        WorldViewProjection[2].z,
        WorldViewProjection[3].z));
    depth = saturate(depth + ShadowBufferZBias);
    color = float4(depth, depth, depth, 1.0) * ColorScale;
}
