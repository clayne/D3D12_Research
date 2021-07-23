#include "Graphics/Light.h"
#include "Core/BitField.h"
#include "Graphics/Core/DescriptorHandle.h"
#include "Graphics/Core/Graphics.h"
#include "ShaderCommon.h"

class ImGuiRenderer;
class Mesh;
struct SubMesh;
struct Material;
class ClusteredForward;
class TiledForward;
class Camera;
class RTAO;
class RTReflections;
class SSAO;
class GpuParticles;
class PathTracing;

enum class DefaultTexture
{
	White2D,
	Black2D,
	Magenta2D,
	Gray2D,
	Normal2D,
	RoughnessMetalness,
	BlackCube,
	ColorNoise256,
	BlueNoise512,
	MAX,
};

struct Batch
{
	enum class Blending
	{
		Opaque = 1,
		AlphaMask = 2,
		AlphaBlend = 4,
	};
	int Index = 0;
	Blending BlendMode = Blending::Opaque;
	const SubMesh* pMesh = nullptr;
	Matrix WorldMatrix;
	BoundingBox LocalBounds;
	BoundingBox Bounds;
	float Radius;
	int Material;
};
DECLARE_BITMASK_TYPE(Batch::Blending)

using VisibilityMask = BitField<2048>;

struct SceneData
{
	Texture* pResolvedDepth = nullptr;
	Texture* pDepthBuffer = nullptr;
	Texture* pRenderTarget = nullptr;
	Texture* pResolvedTarget = nullptr;
	Texture* pPreviousColor = nullptr;
	Texture* pNormals = nullptr;
	Texture* pResolvedNormals = nullptr;
	Texture* pAO = nullptr;
	std::vector<Batch> Batches;
	DescriptorHandle GlobalSRVHeapHandle{};
	Buffer* pLightBuffer = nullptr;
	Buffer* pMaterialBuffer = nullptr;
	Buffer* pMeshBuffer = nullptr;
	Camera* pCamera = nullptr;
	ShaderInterop::ShadowData* pShadowData = nullptr;
	int SceneTLAS = 0;
	int FrameIndex = 0;
	VisibilityMask VisibilityMask;
};

void DrawScene(CommandContext& context, const SceneData& scene, const VisibilityMask& visibility, Batch::Blending blendModes);
void DrawScene(CommandContext& context, const SceneData& scene, Batch::Blending blendModes);

enum class RenderPath
{
	Tiled,
	Clustered,
	PathTracing,
	Visibility,
	MAX
};

class DemoApp
{
public:
	DemoApp(WindowHandle window, const IntVector2& windowRect, int sampleCount = 1);
	~DemoApp();

	void Update();
	void OnResize(int width, int height);

	ImGuiRenderer* GetImGui() const { return m_pImGuiRenderer.get(); }
	Texture* GetDefaultTexture(DefaultTexture type) const { return m_DefaultTextures[(int)type].get(); }
	Texture* GetDepthStencil() const { return m_pDepthStencil.get(); }
	Texture* GetResolvedDepthStencil() const { return m_pResolvedDepthStencil.get(); }
	Texture* GetCurrentRenderTarget() const { return m_SampleCount > 1 ? m_pMultiSampleRenderTarget.get() : m_pHDRRenderTarget.get(); }
	Texture* GetCurrentBackbuffer() const { return m_pSwapchain->GetBackBuffer(); }

	GraphicsDevice* GetDevice() const { return m_pDevice.get(); }

private:
	void InitializePipelines();
	void InitializeAssets(CommandContext& context);
	void SetupScene(CommandContext& context);

	void UpdateImGui();
	void UpdateTLAS(CommandContext& context);

	std::unique_ptr<GraphicsDevice> m_pDevice;
	std::unique_ptr<SwapChain> m_pSwapchain;

	unsigned int m_WindowWidth;
	unsigned int m_WindowHeight;

	int m_Frame = 0;
	std::array<float, 180> m_FrameTimes{};

	std::unique_ptr<Texture> m_pMultiSampleRenderTarget;
	std::unique_ptr<Texture> m_pHDRRenderTarget;
	std::unique_ptr<Texture> m_pPreviousColor;
	std::unique_ptr<Texture> m_pTonemapTarget;
	std::unique_ptr<Texture> m_pDepthStencil;
	std::unique_ptr<Texture> m_pResolvedDepthStencil;
	std::unique_ptr<Texture> m_pTAASource;
	std::unique_ptr<Texture> m_pVelocity;
	std::unique_ptr<Texture> m_pNormals;
	std::unique_ptr<Texture> m_pResolvedNormals;
	std::vector<std::unique_ptr<Texture>> m_ShadowMaps;

	std::unique_ptr<ImGuiRenderer> m_pImGuiRenderer;
	std::unique_ptr<ClusteredForward> m_pClusteredForward;
	std::unique_ptr<TiledForward> m_pTiledForward;
	std::unique_ptr<RTAO> m_pRTAO;
	std::unique_ptr<RTReflections> m_pRTReflections;
	std::unique_ptr<SSAO> m_pSSAO;
	std::unique_ptr<PathTracing> m_pPathTracing;

	std::unique_ptr<Texture> m_pLightCookie;
	std::array<std::unique_ptr<Texture>, (int)DefaultTexture::MAX> m_DefaultTextures;

	int m_SampleCount = 1;
	std::unique_ptr<Camera> m_pCamera;

	std::unique_ptr<Buffer> m_pScreenshotBuffer;
	int32 m_ScreenshotDelay = -1;
	uint32 m_ScreenshotRowPitch = 0;

	RenderPath m_RenderPath = RenderPath::Visibility;

	std::vector<std::unique_ptr<Mesh>> m_Meshes;
	std::unique_ptr<Buffer> m_pTLAS;
	std::unique_ptr<Buffer> m_pTLASScratch;

	//Shadow mapping
	std::unique_ptr<RootSignature> m_pShadowsRS;
	PipelineState* m_pShadowsOpaquePSO = nullptr;
	PipelineState* m_pShadowsAlphaMaskPSO = nullptr;

	//Depth Prepass
	std::unique_ptr<RootSignature> m_pDepthPrepassRS;
	PipelineState* m_pDepthPrepassOpaquePSO = nullptr;
	PipelineState* m_pDepthPrepassAlphaMaskPSO = nullptr;

	//MSAA Depth resolve
	std::unique_ptr<RootSignature> m_pResolveDepthRS;
	PipelineState* m_pResolveDepthPSO = nullptr;

	//Tonemapping
	std::unique_ptr<Texture> m_pDownscaledColor;
	std::unique_ptr<RootSignature> m_pLuminanceHistogramRS;
	PipelineState* m_pLuminanceHistogramPSO = nullptr;
	std::unique_ptr<RootSignature> m_pAverageLuminanceRS;
	PipelineState* m_pAverageLuminancePSO = nullptr;
	std::unique_ptr<RootSignature> m_pToneMapRS;
	PipelineState* m_pToneMapPSO = nullptr;
	PipelineState* m_pDrawHistogramPSO = nullptr;
	std::unique_ptr<RootSignature> m_pDrawHistogramRS;
	std::unique_ptr<Buffer> m_pLuminanceHistogram;
	std::unique_ptr<Buffer> m_pAverageLuminance;
	std::unique_ptr<Texture> m_pDebugHistogramTexture;

	//SSAO
	std::unique_ptr<Texture> m_pAmbientOcclusion;

	//Mip generation
	PipelineState* m_pGenerateMipsPSO = nullptr;
	std::unique_ptr<RootSignature> m_pGenerateMipsRS;

	//Depth Reduction
	PipelineState* m_pPrepareReduceDepthPSO = nullptr;
	PipelineState* m_pPrepareReduceDepthMsaaPSO = nullptr;
	PipelineState* m_pReduceDepthPSO = nullptr;
	std::unique_ptr<RootSignature> m_pReduceDepthRS;
	std::vector<std::unique_ptr<Texture>> m_ReductionTargets;
	std::vector<std::unique_ptr<Buffer>> m_ReductionReadbackTargets;

	//Camera motion
	PipelineState* m_pCameraMotionPSO = nullptr;
	std::unique_ptr<RootSignature> m_pCameraMotionRS;

	//TAA
	PipelineState* m_pTemporalResolvePSO = nullptr;
	std::unique_ptr<RootSignature> m_pTemporalResolveRS;

	//Sky
	std::unique_ptr<RootSignature> m_pSkyboxRS;
	PipelineState* m_pSkyboxPSO = nullptr;

	//Particles
	std::unique_ptr<GpuParticles> m_pParticles;

	//Light data
	std::unique_ptr<Buffer> m_pMaterialBuffer;
	std::unique_ptr<Buffer> m_pMeshBuffer;
	std::vector<Light> m_Lights;
	std::unique_ptr<Buffer> m_pLightBuffer;

	// Visibility buffer
	std::unique_ptr<RootSignature> m_pVisibilityRenderingRS;
	PipelineState* m_pVisibilityRenderingPSO = nullptr;
	std::unique_ptr<Texture> m_pVisibilityTexture;
	std::unique_ptr<Texture> m_pBarycentricsTexture;
	std::unique_ptr<RootSignature> m_pVisibilityShadingRS;
	PipelineState* m_pVisibilityShadingPSO = nullptr;

	Texture* m_pVisualizeTexture = nullptr;
	SceneData m_SceneData;
	bool m_CapturePix = false;
};
