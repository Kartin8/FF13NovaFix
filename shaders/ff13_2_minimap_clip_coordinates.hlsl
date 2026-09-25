// XIII-2's NaviMap pixel shaders clip in the unmodified homogeneous XY
// coordinate system. Keep POSITION corrected by NovaFix, but undo that
// correction only for TEXCOORD2, which feeds s_clipNaviCircle.
float4 wvp0 : register(c0);
float4 wvp1 : register(c1);
float4 wvp3 : register(c3);
// (fill POSITION.y scale, fill POSITION.y translation,
//  native clip Y scale, native clip Y translation).
float4 minimapFillAndClipY : register(c254);
// (1 / scaleX, 1 / scaleY, -translateX / scaleX,
//  -translateY / scaleY), including the GUI safe-area transform.
float4 minimapClipInverse : register(c255);

struct Input {
    float4 position : POSITION0;
    float4 color : COLOR0;
    float2 texcoord : TEXCOORD0;
};

struct Output {
    float4 position : POSITION0;
    float4 color : COLOR0;
    float2 textureCoordinate : TEXCOORD0;
    float2 auxiliaryCoordinate : TEXCOORD1;
    float2 clipCoordinate : TEXCOORD2;
};

Output main(Input input) {
    Output output;
    float4 corrected = wvp0 * input.position.x +
                       wvp1 * input.position.y + wvp3;
    output.position = corrected;
    output.position.y = corrected.y * minimapFillAndClipY.x +
                        corrected.w * minimapFillAndClipY.y;
    output.color = input.color.yzwx;
    output.textureCoordinate = input.position.zw;
    output.auxiliaryCoordinate = input.texcoord;
    float2 nativeClip = corrected.xy * minimapClipInverse.xy +
                        corrected.ww * minimapClipInverse.zw;
    // c254.w is an offset in TEXCOORD2 units, not homogeneous clip space.
    // Multiplying it by corrected.w moves the entire mask off the map.
    output.clipCoordinate = float2(
        nativeClip.x,
        nativeClip.y * minimapFillAndClipY.z +
            minimapFillAndClipY.w);
    return output;
}
