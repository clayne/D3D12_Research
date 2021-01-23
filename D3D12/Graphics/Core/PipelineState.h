#pragma once
#include "GraphicsResource.h"

class Shader;
class ShaderLibrary;

enum class BlendMode
{
	Replace = 0,
	Additive,
	Multiply,
	Alpha,
	AddAlpha,
	PreMultiplyAlpha,
	InverseDestinationAlpha,
	Subtract,
	SubtractAlpha,
	Undefined,
};

enum class PipelineStateType
{
	Graphics,
	Compute,
	Mesh,
	MAX
};

class PipelineState : public GraphicsObject
{
public:
	PipelineState(Graphics* pParent);
	PipelineState(const PipelineState& other);
	~PipelineState();
	ID3D12PipelineState* GetPipelineState() const { return m_pPipelineState.Get(); }
	void Finalize(const char* pName);

	void SetRenderTargetFormat(DXGI_FORMAT rtvFormat, DXGI_FORMAT dsvFormat, uint32 msaa);
	void SetRenderTargetFormats(DXGI_FORMAT* rtvFormats, uint32 count, DXGI_FORMAT dsvFormat, uint32 msaa);

	//BlendState
	void SetBlendMode(const BlendMode& blendMode, bool alphaToCoverage);

	//DepthStencilState
	void SetDepthEnabled(bool enabled);
	void SetDepthWrite(bool enabled);
	void SetDepthTest(const D3D12_COMPARISON_FUNC func);
	void SetStencilTest(bool stencilEnabled, D3D12_COMPARISON_FUNC mode, D3D12_STENCIL_OP pass, D3D12_STENCIL_OP fail, D3D12_STENCIL_OP zFail, unsigned int stencilRef, unsigned char compareMask, unsigned char writeMask);

	//RasterizerState
	void SetFillMode(D3D12_FILL_MODE fillMode);
	void SetCullMode(D3D12_CULL_MODE cullMode);
	void SetLineAntialias(bool lineAntiAlias);
	void SetDepthBias(int depthBias, float depthBiasClamp, float slopeScaledDepthBias);

	void SetInputLayout(D3D12_INPUT_ELEMENT_DESC* pElements, uint32 count);
	void SetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY_TYPE topology);

	void SetRootSignature(ID3D12RootSignature* pRootSignature);

	//Shaders
	void SetVertexShader(Shader* pShader);
	void SetPixelShader(Shader* pShader);
	void SetHullShader(Shader* pShader);
	void SetDomainShader(Shader* pShader);
	void SetGeometryShader(Shader* pShader);
	void SetComputeShader(Shader* pShader);
	void SetMeshShader(Shader* pShader);
	void SetAmplificationShader(Shader* pShader);

	PipelineStateType GetType() const { return m_Type; }

private:
	ComPtr<ID3D12PipelineState> m_pPipelineState;

	CD3DX12_PIPELINE_STATE_STREAM_HELPER m_Desc;

	PipelineStateType m_Type = PipelineStateType::MAX;
};
