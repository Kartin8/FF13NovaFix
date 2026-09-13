sampler2D Diffuse : register(s0);
float4 WorldViewProjection[4] : register(c0);
float4 Mask : register(c4);
float UvIndex : register(c5);
float ShadowBias : register(c6);
float4 ColorScale : register(c7);

void main(float4 worldPosition : TEXCOORD1,
          float4 uvs : TEXCOORD4,
          out float4 color : COLOR0,
          out float outputDepth : DEPTH)
{
    float2 uv = abs(UvIndex) > 0.0 ? uvs.zw : uvs.xy;
    float alpha = dot(tex2D(Diffuse, uv), Mask);
    clip(alpha - 0.15);

    float depth = dot(worldPosition, float4(
        WorldViewProjection[0].z,
        WorldViewProjection[1].z,
        WorldViewProjection[2].z,
        WorldViewProjection[3].z));
    depth = saturate(depth + ShadowBias);
    color = saturate(float4(depth, depth, depth, alpha)) * ColorScale;
    outputDepth = depth;
}
