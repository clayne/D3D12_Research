#include "stdafx.h"
#include "VolumetricFog.h"
#include "Core/Profiler.h"
#include "Core/ConsoleVariables.h"
#include "RHI/PipelineState.h"
#include "RHI/RootSignature.h"
#include "RHI/Buffer.h"
#include "RHI/Device.h"
#include "RHI/CommandContext.h"
#include "RHI/Texture.h"
#include "Renderer/Techniques/LightCulling.h"
#include "Renderer/Renderer.h"
#include "Renderer/Light.h"
#include "RenderGraph/RenderGraph.h"
#include "Scene/World.h"

static constexpr int gVolumetricFroxelTexelSize = 8;
static constexpr int gVolumetricNumZSlices = 128;

VolumetricFog::VolumetricFog(GraphicsDevice* pDevice)
	: m_pDevice(pDevice)
{
	m_pInjectVolumeLightPSO = pDevice->CreateComputePipeline(GraphicsCommon::pCommonRS, "VolumetricFog.hlsl", "InjectFogLightingCS");
	m_pAccumulateVolumeLightPSO = pDevice->CreateComputePipeline(GraphicsCommon::pCommonRS, "VolumetricFog.hlsl", "AccumulateFogCS");
}

VolumetricFog::~VolumetricFog()
{
}

RGTexture* VolumetricFog::RenderFog(RGGraph& graph, const RenderView* pView, const LightCull3DData& lightCullData, VolumetricFogData& fogData)
{
	RG_GRAPH_SCOPE("Volumetric Lighting", graph);

	Array<ShaderInterop::FogVolume> volumes;
	auto fog_view = pView->pWorld->Registry.view<const Transform, const FogVolume>();
	fog_view.each([&](const Transform& transform, const FogVolume& fogVolume)
		{
			ShaderInterop::FogVolume& v = volumes.emplace_back();
			v.Location = transform.Position;
			v.Extents = fogVolume.Extents;
			v.DensityBase = fogVolume.DensityBase;
			v.DensityChange = fogVolume.DensityChange;
			v.Color = fogVolume.Color;
		});

	if (volumes.empty())
		return graph.Import(GraphicsCommon::GetDefaultTexture(DefaultTexture::Black3D));

	TextureDesc volumeDesc = TextureDesc::Create3D(
		Math::DivideAndRoundUp((uint32)pView->GetDimensions().x, gVolumetricFroxelTexelSize),
		Math::DivideAndRoundUp((uint32)pView->GetDimensions().y, gVolumetricFroxelTexelSize),
		gVolumetricNumZSlices,
		ResourceFormat::RGBA16_FLOAT);

	RGTexture* pSourceVolume = graph.TryImport(fogData.pFogHistory, GraphicsCommon::GetDefaultTexture(DefaultTexture::Black3D));
	RGTexture* pTargetVolume = graph.Create("Fog Target", volumeDesc);
	graph.Export(pTargetVolume, &fogData.pFogHistory);


	RGBuffer* pFogVolumes = graph.Create("Fog Volumes", BufferDesc::CreateStructured((uint32)volumes.size(), sizeof(ShaderInterop::FogVolume)));
	RGUtils::DoUpload(graph, pFogVolumes, volumes.data(), (uint32)volumes.size() * sizeof(ShaderInterop::FogVolume));

	graph.AddPass("Inject Volume Lights", RGPassFlag::Compute)
		.Read({ pSourceVolume, lightCullData.pLightGrid, pFogVolumes })
		.Write(pTargetVolume)
		.Bind([=](CommandContext& context, const RGResources& resources)
			{
				Texture* pTarget = resources.Get(pTargetVolume);

				context.SetComputeRootSignature(GraphicsCommon::pCommonRS);
				context.SetPipelineState(m_pInjectVolumeLightPSO);

				struct
				{
					Vector3i ClusterDimensions;
					float Jitter;
					Vector3 InvClusterDimensions;
					float LightClusterSizeFactor;
					Vector2 LightGridParams;
					Vector2i LightClusterDimensions;
					float MinBlendFactor;
					uint32 NumFogVolumes;
				} params;

				params.ClusterDimensions = Vector3i(volumeDesc.Width, volumeDesc.Height, volumeDesc.Depth);
				params.InvClusterDimensions = Vector3(1.0f / volumeDesc.Width, 1.0f / volumeDesc.Height, 1.0f / volumeDesc.Depth);
				constexpr Math::HaltonSequence<32, 2> halton;
				params.Jitter = halton[pView->pRenderer->GetFrameIndex() & 31];
				params.LightClusterSizeFactor = (float)gVolumetricFroxelTexelSize / lightCullData.ClusterSize;
				params.LightGridParams = lightCullData.LightGridParams;
				params.LightClusterDimensions = Vector2i(lightCullData.ClusterCount.x, lightCullData.ClusterCount.y);
				params.MinBlendFactor = pView->CameraCut ? 1.0f : 0.0f;
				params.NumFogVolumes = pFogVolumes->GetDesc().NumElements();

				Renderer::BindViewUniforms(context, *pView);
				context.BindRootCBV(BindingSlot::PerInstance, params);
				context.BindResources(BindingSlot::UAV, pTarget->GetUAV());
				context.BindResources(BindingSlot::SRV, {
					resources.GetSRV(pFogVolumes),
					resources.GetSRV(lightCullData.pLightGrid),
					resources.GetSRV(pSourceVolume),
					});

				context.Dispatch(
					ComputeUtils::GetNumThreadGroups(
						pTarget->GetWidth(), 8,
						pTarget->GetHeight(), 8,
						pTarget->GetDepth(), 4)
				);
			});

	RGTexture* pFinalVolumeFog = graph.Create("Volumetric Fog", volumeDesc);

	graph.AddPass("Accumulate Volume Fog", RGPassFlag::Compute)
		.Read({ pTargetVolume })
		.Write(pFinalVolumeFog)
		.Bind([=](CommandContext& context, const RGResources& resources)
			{
				Texture* pFinalFog = resources.Get(pFinalVolumeFog);

				context.SetComputeRootSignature(GraphicsCommon::pCommonRS);
				context.SetPipelineState(m_pAccumulateVolumeLightPSO);

				struct
				{
					Vector3i ClusterDimensions;
					uint32 : 32;
					Vector3 InvClusterDimensions;
					uint32 : 32;
				} params;
				params.ClusterDimensions = Vector3i(volumeDesc.Width, volumeDesc.Height, volumeDesc.Depth);
				params.InvClusterDimensions = Vector3(1.0f / volumeDesc.Width, 1.0f / volumeDesc.Height, 1.0f / volumeDesc.Depth);

				Renderer::BindViewUniforms(context, *pView);
				context.BindRootCBV(BindingSlot::PerInstance, params);
				context.BindResources(BindingSlot::UAV, pFinalFog->GetUAV());
				context.BindResources(BindingSlot::SRV, {
					resources.GetSRV(pTargetVolume),
					}, 2);

				context.Dispatch(
					ComputeUtils::GetNumThreadGroups(
						pFinalFog->GetWidth(), 8,
						pFinalFog->GetHeight(), 8));
			});
	return pFinalVolumeFog;
}
