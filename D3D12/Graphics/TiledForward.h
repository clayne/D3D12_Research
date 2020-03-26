#pragma once
#include "RenderGraph/RenderGraph.h"
class Graphics;
class ComputePipelineState;
class RootSignature;
class GraphicsPipelineState;
class Texture;
class Camera;
struct Batch;
class CommandContext;
class Buffer;
class UnorderedAccessView;
class RGGraph;

struct TiledForwardInputResources
{
	RGResourceHandle ResolvedDepthBuffer;
	RGResourceHandle DepthBuffer;
	Texture* pShadowMap = nullptr;
	Texture* pRenderTarget = nullptr;
	const std::vector<Batch>* pOpaqueBatches = nullptr;
	const std::vector<Batch>* pTransparantBatches = nullptr;
	Buffer* pLightBuffer = nullptr;
	Camera* pCamera = nullptr;
};

class TiledForward
{
public:
	TiledForward(Graphics* pGraphics);

	void OnSwapchainCreated(int windowWidth, int windowHeight);

	void Execute(RGGraph& graph, const TiledForwardInputResources& resources);

private:
	void SetupResources(Graphics* pGraphics);
	void SetupPipelines(Graphics* pGraphics);

	Graphics* m_pGraphics;

	//Light Culling
	std::unique_ptr<RootSignature> m_pComputeLightCullRS;
	std::unique_ptr<ComputePipelineState> m_pComputeLightCullPSO;
	std::unique_ptr<Buffer> m_pLightIndexCounter;
	UnorderedAccessView* m_pLightIndexCounterRawUAV = nullptr;
	std::unique_ptr<Buffer> m_pLightIndexListBufferOpaque;
	std::unique_ptr<Texture> m_pLightGridOpaque;
	std::unique_ptr<Buffer> m_pLightIndexListBufferTransparant;
	std::unique_ptr<Texture> m_pLightGridTransparant;

	//Diffuse
	std::unique_ptr<RootSignature> m_pDiffuseRS;
	std::unique_ptr<GraphicsPipelineState> m_pDiffusePSO;
	std::unique_ptr<GraphicsPipelineState> m_pDiffuseAlphaPSO;
};