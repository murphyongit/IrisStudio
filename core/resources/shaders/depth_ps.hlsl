struct PS_INPUT
{
    float4 pos : POSITION;
    float2 uv : TEXCOORD0;
    float3 color : COLOR;
};

sampler2D depthTex : register(s10);
float depthBlend : register(c0);
float normalBlend : register(c4);
float onlyDrawViewmodel : register(c8);
float greenscreenBackground : register(c12);

float GetLinearizedDepth(float2 texcoord)
{
    float depth = tex2Dlod(depthTex, float4(texcoord, 0, 0)).x;
    const float N = 1.0f;
    const float farPlane = 1000.0f;
    depth /= farPlane - depth * (farPlane - N);
    return depth;
}

float3 GetScreenSpaceNormal(float2 texcoord)
{
    float3 offset = float3(1.0f / 2560.0f, 1.0f / 1088.0f, 0.0f);
    float2 posCenter = texcoord.xy;
    float2 posNorth  = posCenter - offset.zy;
    float2 posEast   = posCenter + offset.xz;

    float3 vertCenter = float3(posCenter - 0.5f, 1.0f) * GetLinearizedDepth(posCenter);
    float3 vertNorth  = float3(posNorth - 0.5f,  1.0f) * GetLinearizedDepth(posNorth);
    float3 vertEast   = float3(posEast - 0.5f,   1.0f) * GetLinearizedDepth(posEast);

    return normalize(cross(vertCenter - vertNorth, vertCenter - vertEast)) * 0.5f + 0.5f;
}

float4 main(PS_INPUT input) : COLOR
{
    // drawn additively: green where nothing was drawn, so the effects in front of the background are kept
    if (greenscreenBackground > 0.0f)
    {
        float rawDepth = tex2Dlod(depthTex, float4(input.uv, 0, 0)).x;
        return rawDepth >= 0.999999f ? float4(0.0f, 1.0f, 0.0f, 1.0f) : float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    float3 normal = GetScreenSpaceNormal(input.uv);
    float depth = GetLinearizedDepth(input.uv);

    if (onlyDrawViewmodel > 0.0f) 
    {
        if (depth < 0.001f)
        {
            float4 result = lerp(float4(0.0f, 0.0f, 0.0f, 0.0f), float4(0.0f, 0.0f, 0.0f, 1.0f), depthBlend);
            return lerp(result, float4(normal, 1.0f), normalBlend);
        }
        else 
        {
            float4 result = lerp(float4(0.0f, 1.0f, 0.0f, 1.0f), float4(1.0f, 1.0f, 1.0f, 1.0f), depthBlend);
            return lerp(result, float4(0.0f, 0.0f, 0.0f, 1.0f), normalBlend);
        }
	}
    
    float4 result = lerp(float4(0.0f, 0.0f, 0.0f, 0.0f), float4(depth, depth, depth, 1.0f), depthBlend);
    result = lerp(result, float4(normal, 1.0f), normalBlend);
    return result;
}