#pragma once
class Graphics;
class RootSignature;
class Texture;
class Camera;
class RGGraph;
class PipelineState;

class SSAO
{
public:
	SSAO(Graphics* pGraphics);

	void OnSwapchainCreated(int windowWidth, int windowHeight);

	void Execute(RGGraph& graph, Texture* pColor, Texture* pDepth, Camera& camera);

private:
	void SetupResources(Graphics* pGraphics);
	void SetupPipelines(Graphics* pGraphics);

	std::unique_ptr<Texture> m_pAmbientOcclusionIntermediate;
	std::unique_ptr<RootSignature> m_pSSAORS;
	PipelineState* m_pSSAOPSO = nullptr;
	std::unique_ptr<RootSignature> m_pSSAOBlurRS;
	PipelineState* m_pSSAOBlurPSO = nullptr;
};

