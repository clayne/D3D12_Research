#pragma once
#include "../RenderGraph/RenderGraphDefinitions.h"
class GraphicsDevice;
class PipelineState;
class RootSignature;
class Texture;
class CommandSignature;
class Buffer;
class UnorderedAccessView;
class RGGraph;
struct SceneView;
struct SceneTextures;

struct ClusteredLightCullData
{
	IntVector3 ClusterCount;
	RGHandle<Buffer> AABBs;
	RGHandle<Buffer> LightIndexGrid;
	RGHandle<Buffer> LightGrid;

	Vector2 LightGridParams;

	RefCountPtr<Buffer> pDebugLightGrid;
	Matrix DebugClustersViewMatrix;
	bool DirtyDebugData = true;
};

struct VolumetricFogData
{
	RefCountPtr<Texture> pFogHistory;
};

class ClusteredForward
{
public:
	ClusteredForward(GraphicsDevice* pDevice);
	~ClusteredForward();

	void ComputeLightCulling(RGGraph& graph, const SceneView& view, ClusteredLightCullData& resources);
	void VisualizeClusters(RGGraph& graph, const SceneView& view, SceneTextures& sceneTextures, ClusteredLightCullData& resources);

	RGHandle<Texture> RenderVolumetricFog(RGGraph& graph, const SceneView& view, const ClusteredLightCullData& cullData, VolumetricFogData& fogData);

	void RenderBasePass(RGGraph& graph, const SceneView& view, SceneTextures& sceneTextures, const ClusteredLightCullData& lightCullData, RGHandle<Texture> fogTexture);

	void Execute(RGGraph& graph, const SceneView& view, SceneTextures& sceneTextures);
	void VisualizeLightDensity(RGGraph& graph, const SceneView& view, SceneTextures& sceneTextures);

private:
	GraphicsDevice* m_pDevice;

	RefCountPtr<Texture> m_pHeatMapTexture;

	ClusteredLightCullData m_LightCullData;
	VolumetricFogData m_VolumetricFogData;

	// AABB
	RefCountPtr<PipelineState> m_pCreateAabbPSO;
	// Light Culling
	RefCountPtr<RootSignature> m_pLightCullingRS;
	RefCountPtr<PipelineState> m_pLightCullingPSO;

	// Lighting
	RefCountPtr<RootSignature> m_pDiffuseRS;
	RefCountPtr<PipelineState> m_pDiffusePSO;
	RefCountPtr<PipelineState> m_pDiffuseMaskedPSO;
	RefCountPtr<PipelineState> m_pDiffuseTransparancyPSO;

	RefCountPtr<PipelineState> m_pMeshShaderDiffusePSO;
	RefCountPtr<PipelineState> m_pMeshShaderDiffuseMaskedPSO;
	RefCountPtr<PipelineState> m_pMeshShaderDiffuseTransparancyPSO;

	//Cluster debug rendering
	RefCountPtr<RootSignature> m_pVisualizeLightClustersRS;
	RefCountPtr<PipelineState> m_pVisualizeLightClustersPSO;

	//Visualize Light Count
	RefCountPtr<RootSignature> m_pVisualizeLightsRS;
	RefCountPtr<PipelineState> m_pVisualizeLightsPSO;

	//Volumetric Fog
	RefCountPtr<RootSignature> m_pVolumetricLightingRS;
	RefCountPtr<PipelineState> m_pInjectVolumeLightPSO;
	RefCountPtr<PipelineState> m_pAccumulateVolumeLightPSO;
};

