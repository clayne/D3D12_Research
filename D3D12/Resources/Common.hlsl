struct Light
{
	float3 Position;
	int Enabled;
	float3 Direction;
	int Type;
	float Range;
	float SpotLightAngle;
	float Attenuation;
	float Intensity;
	float4 Color;
};

struct Plane
{
    float3 Normal;
    float DistanceToOrigin;
};

struct Frustum
{
    Plane Planes[4];
};

struct Sphere
{
    float3 Position;
    float Radius;
};

struct Cone
{
    float3 Tip;
    float Height;
    float3 Direction;
    float Radius;
};

struct AABB
{
    float3 Center;
    float3 Extents;
};

Plane CalculatePlane(float3 a, float3 b, float3 c)
{
    float3 v0 = b - a;
    float3 v1 = c - a;
    
    Plane plane;
    plane.Normal = normalize(cross(v1, v0));
    plane.DistanceToOrigin = dot(plane.Normal, a);
    return plane;
}

// Convert clip space coordinates to view space
float4 ClipToView(float4 clip, float4x4 projectionInverse)
{
    // View space position.
    float4 view = mul(clip, projectionInverse);
    // Perspective projection.
    view = view / view.w;
    return view;
}
 
// Convert screen space coordinates to view space.
float4 ScreenToView(float4 screen, float2 screenDimensions, float4x4 projectionInverse)
{
    // Convert to normalized texture coordinates
    float2 texCoord = screen.xy / screenDimensions;
    // Convert to clip space
    float4 clip = float4(float2(texCoord.x, 1.0f - texCoord.y) * 2.0f - 1.0f, screen.z, screen.w);
    return ClipToView(clip, projectionInverse);
}

void AABBFromMinMax(inout AABB aabb, float3 minimum, float3 maximum)
{
    aabb.Center = (minimum + maximum) / 2.0f;
    aabb.Extents = abs(maximum - aabb.Center);
}

float3 HUEtoRGB(in float H)
{
    float R = abs(H * 6 - 3) - 1;
    float G = 2 - abs(H * 6 - 2);
    float B = 2 - abs(H * 6 - 4);
    return saturate(float3(R,G,B));
}