#include "Common.hlsli"
#include "Random.hlsli"

#define BLOCK_SIZE 16

struct PassData
{
	float AoPower;
	float AoRadius;
	float AoDepthThreshold;
	int AoSamples;

	Texture2DH<float> DepthTexture;
	RWTexture2DH<float> OutputTexture;
};

DEFINE_CONSTANTS(PassData, 0);

float3x3 TangentMatrix(float3 z)
{
    float3 ref = abs(dot(z, float3(0, 1, 0))) > 0.99f ? float3(0, 0, 1) : float3(0, 1, 0);
    float3 x = normalize(cross(ref, z));
    float3 y = cross(z, x);
    return float3x3(x, y, z);
}

[numthreads(BLOCK_SIZE, BLOCK_SIZE, 1)]
void CSMain(uint3 threadId : SV_DispatchThreadID)
{
	if(any(threadId.xy >= cView.ViewportDimensions))
		return;

	PassData cPass = cPassData;

	float2 uv = TexelToUV(threadId.xy, cView.ViewportDimensionsInv);
	float depth = cPass.DepthTexture.SampleLevel(sPointClamp, uv, 0);
	float3 viewNormal = ViewNormalFromDepth(uv, cPass.DepthTexture.Get(), NormalReconstructMethod::Taps5);
	float3 viewPos = ViewPositionFromDepth(uv.xy, depth, cView.ClipToView).xyz;

	uint seed = SeedThread(threadId.xy, cView.ViewportDimensions, cView.FrameIndex);
	float3 randomVec = float3(Random01(seed), Random01(seed), Random01(seed)) * 2.0f - 1.0f;
	float3x3 TBN = TangentMatrix(viewNormal);

	// Diffuse reflections integral is over (1 / PI) * Li * NdotL
	// We sample a cosine weighted distribution over the hemisphere which has a PDF which conveniently cancels out the inverse PI and NdotL terms.

	float occlusion = 0;

	for(int i = 0; i < cPass.AoSamples; ++i)
	{
		float2 u = float2(Random01(seed), Random01(seed));
		float pdf;
		float3 hemispherePoint = HemisphereSampleCosineWeight(u, pdf);
		float3 vpos = viewPos + mul(hemispherePoint, TBN) * cPass.AoRadius;
		float4 newTexCoord = mul(float4(vpos, 1), cView.ViewToClip);
		newTexCoord.xyz /= newTexCoord.w;
		newTexCoord.xy = ClipToUV(newTexCoord.xy);

		// Make sure we're not sampling outside the screen
		if(all(newTexCoord.xy >= 0) && all(newTexCoord.xy <= 1))
		{
			float sampleDepth = cPass.DepthTexture.SampleLevel(sPointClamp, newTexCoord.xy, 0).r;
			float depthVpos = LinearizeDepth(sampleDepth);
			float rangeCheck = smoothstep(0.0f, 1.0f, cPass.AoRadius / (viewPos.z - depthVpos));
			occlusion += (vpos.z >= depthVpos + cPass.AoDepthThreshold) * rangeCheck;
		}
	}
	occlusion = occlusion / cPass.AoSamples;
	cPass.OutputTexture.Store(threadId.xy, pow(saturate(1 - occlusion), cPass.AoPower));
}
