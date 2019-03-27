struct LightResult
{
	float4 Diffuse;
	float4 Specular;
};

float GetSpecularBlinnPhong(float3 viewDirection, float3 normal, float3 lightVector, float shininess)
{
	float3 hv = normalize(lightVector - viewDirection);
	float specularStrength = dot(hv, normal);
	return pow(saturate(specularStrength), shininess);
}

float GetSpecularPhong(float3 viewDirection, float3 normal, float3 lightVector, float shininess)
{
	float3 reflectedLight = reflect(-lightVector, normal);
	float specularStrength = max(0, dot(reflectedLight, -viewDirection));
	return pow((specularStrength), shininess);
}

float4 DoDiffuse(Light light, float3 normal, float3 lightVector)
{
	return float4(light.Color.rgb * max(dot(normal, lightVector), 0), 1);
}

float4 DoSpecular(Light light, float3 normal, float3 lightVector, float3 viewDirection)
{
	return float4(light.Color.rgb * GetSpecularPhong(viewDirection, normal, lightVector, 20.0f), 1);
}

float DoAttenuation(Light light, float d)
{
    return 1.0f - smoothstep(light.Range * light.Attenuation, light.Range, d);
}

LightResult DoPointLight(Light light, float3 worldPosition, float3 normal, float3 viewDirection)
{
	LightResult result;
	float3 L = light.Position - worldPosition;
	float d = length(L);
	L = L / d;

	float attenuation = DoAttenuation(light, d);
	result.Diffuse = light.Color.w * attenuation * DoDiffuse(light, normal, L);
	result.Specular = light.Color.w * attenuation * DoSpecular(light, normal, L, viewDirection);
	return result;
}

LightResult DoDirectionalLight(Light light, float3 normal, float3 viewDirection)
{
	LightResult result;
	result.Diffuse = light.Color.w * DoDiffuse(light, normal, -light.Direction);
	result.Specular = light.Color.w * DoSpecular(light, normal, -light.Direction, viewDirection);
	return result;
}

LightResult DoSpotLight(Light light, float3 worldPosition, float3 normal, float3 viewDirection)
{
	LightResult result = (LightResult)0;
	float3 L = light.Position - worldPosition;
	float d = length(L);
	L = L / d;

	float minCos = cos(radians(light.SpotLightAngle));
	float maxCos = lerp(minCos, 1.0f, 1 - light.Attenuation);
	float cosAngle = dot(-L, light.Direction);
	float spotIntensity = smoothstep(minCos, maxCos, cosAngle);

	float attenuation = DoAttenuation(light, d);

	result.Diffuse = light.Color.w * attenuation * spotIntensity * DoDiffuse(light, normal, L);
	result.Specular = light.Color.w * attenuation * spotIntensity * DoSpecular(light, normal, L, viewDirection);
	return result;
}