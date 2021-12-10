#include "stdafx.h"
#include "Graphics.h"
#include "CommandQueue.h"
#include "CommandContext.h"
#include "OfflineDescriptorAllocator.h"
#include "GraphicsResource.h"
#include "RootSignature.h"
#include "PipelineState.h"
#include "Shader.h"
#include "DynamicResourceAllocator.h"
#include "Graphics/Mesh.h"
#include "Core/Input.h"
#include "Texture.h"
#include "Scene/Camera.h"
#include "ResourceViews.h"
#include "Buffer.h"
#include "Graphics/Profiler.h"
#include "StateObject.h"
#include "Core/CommandLine.h"
#include "pix3.h"

// Setup the Agility D3D12 SDK
extern "C" { _declspec(dllexport) extern const UINT D3D12SDKVersion = D3D12_SDK_VERSION; }
extern "C" { _declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }

const DXGI_FORMAT GraphicsDevice::DEPTH_STENCIL_FORMAT = DXGI_FORMAT_D32_FLOAT;
const DXGI_FORMAT GraphicsDevice::RENDER_TARGET_FORMAT = DXGI_FORMAT_R11G11B10_FLOAT;

std::unique_ptr<GraphicsInstance> GraphicsInstance::CreateInstance(GraphicsInstanceFlags createFlags /*= GraphicsFlags::None*/)
{
	return std::make_unique<GraphicsInstance>(createFlags);
}

GraphicsInstance::GraphicsInstance(GraphicsInstanceFlags createFlags)
{
	UINT flags = 0;
	if (EnumHasAnyFlags(createFlags, GraphicsInstanceFlags::DebugDevice))
	{
		flags |= DXGI_CREATE_FACTORY_DEBUG;
	}
	VERIFY_HR(CreateDXGIFactory2(flags, IID_PPV_ARGS(m_pFactory.GetAddressOf())));
	BOOL allowTearing;
	if (SUCCEEDED(m_pFactory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(BOOL))))
	{
		m_AllowTearing = allowTearing;
	}

	if (EnumHasAnyFlags(createFlags, GraphicsInstanceFlags::DebugDevice))
	{
		ComPtr<ID3D12Debug> pDebugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&pDebugController))))
		{
			pDebugController->EnableDebugLayer();
			E_LOG(Warning, "D3D12 Debug Layer Enabled");
		}
	}

	if (EnumHasAnyFlags(createFlags, GraphicsInstanceFlags::DRED))
	{
		ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> pDredSettings;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&pDredSettings))))
		{
			// Turn on auto-breadcrumbs and page fault reporting.
			pDredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			pDredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			pDredSettings->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			E_LOG(Warning, "DRED Enabled");
		}
	}

	if (EnumHasAnyFlags(createFlags, GraphicsInstanceFlags::GpuValidation))
	{
		ComPtr<ID3D12Debug1> pDebugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&pDebugController))))
		{
			pDebugController->SetEnableGPUBasedValidation(true);
			E_LOG(Warning, "D3D12 GPU Based Validation Enabled");
		}
	}

	if (EnumHasAnyFlags(createFlags, GraphicsInstanceFlags::Pix))
	{
		if(PIXLoadLatestWinPixGpuCapturerLibrary())
		{
			E_LOG(Warning, "Dynamically loaded PIX");
		}
	}
}

std::unique_ptr<SwapChain> GraphicsInstance::CreateSwapchain(GraphicsDevice* pDevice, WindowHandle pNativeWindow, DXGI_FORMAT format, uint32 width, uint32 height, uint32 numFrames, bool vsync)
{
	return std::make_unique<SwapChain>(pDevice, m_pFactory.Get(), pNativeWindow, format, width, height, numFrames, vsync);
}

ComPtr<IDXGIAdapter4> GraphicsInstance::EnumerateAdapter(bool useWarp)
{
	ComPtr<IDXGIAdapter4> pAdapter;
	ComPtr<ID3D12Device> pDevice;
	if (!useWarp)
	{
		uint32 adapterIndex = 0;
		E_LOG(Info, "Adapters:");
		DXGI_GPU_PREFERENCE gpuPreference = DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
		while (m_pFactory->EnumAdapterByGpuPreference(adapterIndex++, gpuPreference, IID_PPV_ARGS(pAdapter.ReleaseAndGetAddressOf())) == S_OK)
		{
			DXGI_ADAPTER_DESC3 desc;
			pAdapter->GetDesc3(&desc);
			E_LOG(Info, "\t%s - %f GB", UNICODE_TO_MULTIBYTE(desc.Description), (float)desc.DedicatedVideoMemory * Math::BytesToGigaBytes);

			uint32 outputIndex = 0;
			ComPtr<IDXGIOutput> pOutput;
			while (pAdapter->EnumOutputs(outputIndex++, pOutput.ReleaseAndGetAddressOf()) == S_OK)
			{
				ComPtr<IDXGIOutput6> pOutput1;
				pOutput.As(&pOutput1);
				DXGI_OUTPUT_DESC1 outputDesc;
				pOutput1->GetDesc1(&outputDesc);

				E_LOG(Info, "\t\tMonitor %d - %dx%d - HDR: %s - %d BPP - Min Lum %f - Max Lum %f - MaxFFL %f",
					outputIndex,
					outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left,
					outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top,
					outputDesc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? "Yes" : "No",
					outputDesc.BitsPerColor,
					outputDesc.MinLuminance,
					outputDesc.MaxLuminance,
					outputDesc.MaxFullFrameLuminance);
			}
		}
		m_pFactory->EnumAdapterByGpuPreference(0, gpuPreference, IID_PPV_ARGS(pAdapter.GetAddressOf()));
		DXGI_ADAPTER_DESC3 desc;
		pAdapter->GetDesc3(&desc);
		E_LOG(Info, "Using %s", UNICODE_TO_MULTIBYTE(desc.Description));

		constexpr D3D_FEATURE_LEVEL featureLevels[] =
		{
			D3D_FEATURE_LEVEL_12_2,
			D3D_FEATURE_LEVEL_12_1,
			D3D_FEATURE_LEVEL_12_0,
			D3D_FEATURE_LEVEL_11_1,
			D3D_FEATURE_LEVEL_11_0
		};

		VERIFY_HR(D3D12CreateDevice(pAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&pDevice)));
		D3D12_FEATURE_DATA_FEATURE_LEVELS caps{};
		caps.pFeatureLevelsRequested = featureLevels;
		caps.NumFeatureLevels = ARRAYSIZE(featureLevels);
		VERIFY_HR(pDevice->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &caps, sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS)));
		VERIFY_HR(D3D12CreateDevice(pAdapter.Get(), caps.MaxSupportedFeatureLevel, IID_PPV_ARGS(pDevice.ReleaseAndGetAddressOf())));
	}

	if (!pDevice)
	{
		E_LOG(Warning, "No D3D12 Adapter selected. Falling back to WARP");
		m_pFactory->EnumWarpAdapter(IID_PPV_ARGS(pAdapter.GetAddressOf()));
	}
	return pAdapter;
}

std::unique_ptr<GraphicsDevice> GraphicsInstance::CreateDevice(ComPtr<IDXGIAdapter4> pAdapter)
{
	return std::make_unique<GraphicsDevice>(pAdapter.Get());
}

GraphicsDevice::GraphicsDevice(IDXGIAdapter4* pAdapter)
	: m_DeleteQueue(this)
{
	VERIFY_HR(D3D12CreateDevice(pAdapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(m_pDevice.ReleaseAndGetAddressOf())));
	m_pDevice.As(&m_pRaytracingDevice);
	D3D::SetObjectName(m_pDevice.Get(), "Main Device");

	Capabilities.Initialize(this);

	auto OnDeviceRemovedCallback = [](void* pContext, BOOLEAN) {
		GraphicsDevice* pDevice = (GraphicsDevice*)pContext;
		std::string error = D3D::GetErrorString(DXGI_ERROR_DEVICE_REMOVED, pDevice->m_pDevice.Get());
		E_LOG(Error, "%s", error.c_str());
	};

	m_pDeviceRemovalFence = std::make_unique<Fence>(this, UINT64_MAX, "Device Removed Fence");
	m_DeviceRemovedEvent = CreateEventA(nullptr, false, false, nullptr);
	m_pDeviceRemovalFence->GetFence()->SetEventOnCompletion(UINT64_MAX, m_DeviceRemovedEvent);
	RegisterWaitForSingleObject(&m_DeviceRemovedEvent, m_DeviceRemovedEvent, OnDeviceRemovedCallback, this, INFINITE, 0);

	ComPtr<ID3D12InfoQueue> pInfoQueue;
	if (SUCCEEDED(m_pDevice->QueryInterface(IID_PPV_ARGS(pInfoQueue.GetAddressOf()))))
	{
		// Suppress whole categories of messages
		//D3D12_MESSAGE_CATEGORY Categories[] = {};

		// Suppress messages based on their severity level
		D3D12_MESSAGE_SEVERITY Severities[] =
		{
			D3D12_MESSAGE_SEVERITY_INFO
		};

		// Suppress individual messages by their ID
		D3D12_MESSAGE_ID DenyIds[] =
		{
			// This occurs when there are uninitialized descriptors in a descriptor table, even when a
			// shader does not access the missing descriptors.  I find this is common when switching
			// shader permutations and not wanting to change much code to reorder resources.
			D3D12_MESSAGE_ID_INVALID_DESCRIPTOR_HANDLE,
		};

		D3D12_INFO_QUEUE_FILTER NewFilter = {};
		//NewFilter.DenyList.NumCategories = _countof(Categories);
		//NewFilter.DenyList.pCategoryList = Categories;
		NewFilter.DenyList.NumSeverities = _countof(Severities);
		NewFilter.DenyList.pSeverityList = Severities;
		NewFilter.DenyList.NumIDs = _countof(DenyIds);
		NewFilter.DenyList.pIDList = DenyIds;

		if (CommandLine::GetBool("d3dbreakvalidation"))
		{
			VERIFY_HR_EX(pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true), GetDevice());
			E_LOG(Warning, "D3D Validation Break on Severity Enabled");
		}
		pInfoQueue->PushStorageFilter(&NewFilter);

		ComPtr<ID3D12InfoQueue1> pInfoQueue1;
		if (SUCCEEDED(pInfoQueue.As(&pInfoQueue1)))
		{
			auto MessageCallback = [](
				D3D12_MESSAGE_CATEGORY Category,
				D3D12_MESSAGE_SEVERITY Severity,
				D3D12_MESSAGE_ID ID,
				LPCSTR pDescription,
				void* pContext)
			{
				E_LOG(Warning, "D3D12 Validation Layer: %s", pDescription);
			};

			DWORD callbackCookie = 0;
			VERIFY_HR(pInfoQueue1->RegisterMessageCallback(MessageCallback, D3D12_MESSAGE_CALLBACK_FLAG_NONE, this, &callbackCookie));
		}
	}

	bool setStablePowerState = CommandLine::GetBool("stablepowerstate");
	if (setStablePowerState)
	{
		D3D12EnableExperimentalFeatures(0, nullptr, nullptr, nullptr);
		m_pDevice->SetStablePowerState(TRUE);
	}

	//Create all the required command queues
	m_CommandQueues[D3D12_COMMAND_LIST_TYPE_DIRECT] = std::make_unique<CommandQueue>(this, D3D12_COMMAND_LIST_TYPE_DIRECT);
	m_CommandQueues[D3D12_COMMAND_LIST_TYPE_COMPUTE] = std::make_unique<CommandQueue>(this, D3D12_COMMAND_LIST_TYPE_COMPUTE);
	m_CommandQueues[D3D12_COMMAND_LIST_TYPE_COPY] = std::make_unique<CommandQueue>(this, D3D12_COMMAND_LIST_TYPE_COPY);

	// Allocators
	m_pDynamicAllocationManager = std::make_unique<DynamicAllocationManager>(this, BufferFlag::Upload);
	m_pGlobalViewHeap = std::make_unique<GlobalOnlineDescriptorHeap>(this, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2000, 1000000);
	m_pPersistentViewHeap = std::make_unique<PersistentDescriptorAllocator>(m_pGlobalViewHeap.get());
	m_pGlobalSamplerHeap = std::make_unique<GlobalOnlineDescriptorHeap>(this, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 64, 2048);
	m_pPersistentSamplerHeap = std::make_unique<PersistentDescriptorAllocator>(m_pGlobalSamplerHeap.get());

	check(m_DescriptorHeaps.size() == D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES);
	m_DescriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV] = std::make_unique<OfflineDescriptorAllocator>(this, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 256);
	m_DescriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER] = std::make_unique<OfflineDescriptorAllocator>(this, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 128);
	m_DescriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_RTV] = std::make_unique<OfflineDescriptorAllocator>(this, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 128);
	m_DescriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_DSV] = std::make_unique<OfflineDescriptorAllocator>(this, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 64);

	m_pIndirectDispatchSignature = std::make_unique<CommandSignature>(this);
	m_pIndirectDispatchSignature->AddDispatch();
	m_pIndirectDispatchSignature->Finalize("Default Indirect Dispatch");

	m_pIndirectDrawSignature = std::make_unique<CommandSignature>(this);
	m_pIndirectDrawSignature->AddDraw();
	m_pIndirectDrawSignature->Finalize("Default Indirect Draw");

	m_pIndirectDispatchMeshSignature = std::make_unique<CommandSignature>(this);
	m_pIndirectDispatchMeshSignature->AddDispatchMesh();
	m_pIndirectDispatchMeshSignature->Finalize("Default Indirect Dispatch Mesh");

	uint8 smMaj, smMin;
	Capabilities.GetShaderModel(smMaj, smMin);
	m_pShaderManager = std::make_unique<ShaderManager>(smMaj, smMin);
	m_pShaderManager->AddIncludeDir("Resources/Shaders/");
	m_pShaderManager->AddIncludeDir("Graphics/Core/");
}

GraphicsDevice::~GraphicsDevice()
{
	IdleGPU();
	check(UnregisterWait(m_DeviceRemovedEvent) != 0);
}

CommandQueue* GraphicsDevice::GetCommandQueue(D3D12_COMMAND_LIST_TYPE type) const
{
	return m_CommandQueues.at(type).get();
}

CommandContext* GraphicsDevice::AllocateCommandContext(D3D12_COMMAND_LIST_TYPE type)
{
	int typeIndex = (int)type;
	CommandContext* pContext = nullptr;

	{
		std::scoped_lock<std::mutex> lock(m_ContextAllocationMutex);
		if (m_FreeCommandLists[typeIndex].size() > 0)
		{
			pContext = m_FreeCommandLists[typeIndex].front();
			m_FreeCommandLists[typeIndex].pop();
			pContext->Reset();
		}
		else
		{
			ComPtr<ID3D12CommandList> pCommandList;
			ID3D12CommandAllocator* pAllocator = m_CommandQueues[type]->RequestAllocator();
			VERIFY_HR(GetDevice()->CreateCommandList(0, type, pAllocator, nullptr, IID_PPV_ARGS(pCommandList.GetAddressOf())));
			D3D::SetObjectName(pCommandList.Get(), Sprintf("Pooled Commandlist %d", m_CommandLists.size()).c_str());
			m_CommandLists.push_back(std::move(pCommandList));
			m_CommandListPool[typeIndex].emplace_back(std::make_unique<CommandContext>(this, static_cast<ID3D12GraphicsCommandList*>(m_CommandLists.back().Get()), type, m_pGlobalViewHeap.get(), m_pDynamicAllocationManager.get(), pAllocator));
			pContext = m_CommandListPool[typeIndex].back().get();
		}
	}
	return pContext;
}

void GraphicsDevice::FreeCommandList(CommandContext* pCommandList)
{
	std::lock_guard<std::mutex> lockGuard(m_ContextAllocationMutex);
	m_FreeCommandLists[(int)pCommandList->GetType()].push(pCommandList);
}

bool GraphicsDevice::IsFenceComplete(uint64 fenceValue)
{
	D3D12_COMMAND_LIST_TYPE type = (D3D12_COMMAND_LIST_TYPE)(fenceValue >> 56);
	CommandQueue* pQueue = GetCommandQueue(type);
	return pQueue->GetFence()->IsComplete(fenceValue);
}

void GraphicsDevice::WaitForFence(uint64 fenceValue)
{
	D3D12_COMMAND_LIST_TYPE type = (D3D12_COMMAND_LIST_TYPE)(fenceValue >> 56);
	CommandQueue* pQueue = GetCommandQueue(type);
	pQueue->WaitForFence(fenceValue);
}

void GraphicsDevice::TickFrame()
{
	m_DeleteQueue.Clean();
}

void GraphicsDevice::IdleGPU()
{
	for (auto& pCommandQueue : m_CommandQueues)
	{
		if (pCommandQueue)
		{
			pCommandQueue->WaitForIdle();
		}
	}
}

DescriptorHandle GraphicsDevice::StoreViewDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE view)
{
	DescriptorHandle handle = m_pPersistentViewHeap->Allocate();
	m_pDevice->CopyDescriptorsSimple(1, handle.CpuHandle, view, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	return handle;
}

void GraphicsDevice::FreeViewDescriptor(DescriptorHandle& handle)
{
	if (handle.HeapIndex != DescriptorHandle::InvalidHeapIndex)
	{
		m_pPersistentViewHeap->Free(handle.HeapIndex);
	}
}

std::unique_ptr<Texture> GraphicsDevice::CreateTexture(const TextureDesc& desc, const char* pName)
{
	return std::make_unique<Texture>(this, desc, pName);
}

std::unique_ptr<Buffer> GraphicsDevice::CreateBuffer(const BufferDesc& desc, const char* pName)
{
	return std::make_unique<Buffer>(this, desc, pName);
}

ID3D12Resource* GraphicsDevice::CreateResource(const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES initialState, D3D12_HEAP_TYPE heapType, D3D12_CLEAR_VALUE* pClearValue /*= nullptr*/)
{
	ID3D12Resource* pResource;
	D3D12_HEAP_PROPERTIES properties = CD3DX12_HEAP_PROPERTIES(heapType);
	VERIFY_HR_EX(GetDevice()->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, initialState, pClearValue, IID_PPV_ARGS(&pResource)), GetDevice());
	return pResource;
}

void GraphicsDevice::ReleaseResource(ID3D12Resource* pResource)
{
	m_DeleteQueue.EnqueueResource(pResource, GetCommandQueue(D3D12_COMMAND_LIST_TYPE_DIRECT)->GetFence());
}

PipelineState* GraphicsDevice::CreatePipeline(const PipelineStateInitializer& psoDesc)
{
	std::unique_ptr<PipelineState> pPipeline = std::make_unique<PipelineState>(this);
	pPipeline->Create(psoDesc);
	m_Pipelines.push_back(std::move(pPipeline));
	return m_Pipelines.back().get();
}

StateObject* GraphicsDevice::CreateStateObject(const StateObjectInitializer& stateDesc)
{
	std::unique_ptr<StateObject> pStateObject = std::make_unique<StateObject>(this);
	pStateObject->Create(stateDesc);
	m_StateObjects.push_back(std::move(pStateObject));
	return m_StateObjects.back().get();
}

Shader* GraphicsDevice::GetShader(const char* pShaderPath, ShaderType shaderType, const char* pEntryPoint, const std::vector<ShaderDefine>& defines /*= {}*/)
{
	return m_pShaderManager->GetShader(pShaderPath, shaderType, pEntryPoint, defines);
}

ShaderLibrary* GraphicsDevice::GetLibrary(const char* pShaderPath, const std::vector<ShaderDefine>& defines /*= {}*/)
{
	return m_pShaderManager->GetLibrary(pShaderPath, defines);
}

DeferredDeleteQueue::DeferredDeleteQueue(GraphicsDevice* pParent)
	: GraphicsObject(pParent)
{
}

DeferredDeleteQueue::~DeferredDeleteQueue()
{
	GetParent()->IdleGPU();
	Clean();
	check(m_DeletionQueue.empty());
}

void DeferredDeleteQueue::EnqueueResource(ID3D12Object* pResource, Fence* pFence)
{
	std::scoped_lock<std::mutex> lock(m_QueueCS);
	FencedObject object;
	object.pFence = pFence;
	object.FenceValue = pFence->GetCurrentValue();
	object.pResource = pResource;
	m_DeletionQueue.push(object);
}

void DeferredDeleteQueue::Clean()
{
	std::scoped_lock<std::mutex> lock(m_QueueCS);
	while (!m_DeletionQueue.empty())
	{
		const FencedObject& p = m_DeletionQueue.front();
		if (!p.pFence->IsComplete(p.FenceValue))
		{
			break;
		}
		p.pResource->Release();
		m_DeletionQueue.pop();
	}
}

void GraphicsCapabilities::Initialize(GraphicsDevice* pDevice)
{
	m_pDevice = pDevice;

	check(m_FeatureSupport.Init(pDevice->GetDevice()) == S_OK);
	checkf(m_FeatureSupport.ResourceHeapTier() >= D3D12_RESOURCE_HEAP_TIER_2, "Device does not support Resource Heap Tier 2 or higher. Tier 1 is not supported");
	checkf(m_FeatureSupport.ResourceBindingTier() >= D3D12_RESOURCE_BINDING_TIER_3, "Device does not support Resource Binding Tier 3 or higher. Tier 2 and under is not supported.");

	RenderPassTier = m_FeatureSupport.RenderPassesTier();
	RayTracingTier = m_FeatureSupport.RaytracingTier();
	VRSTier = m_FeatureSupport.VariableShadingRateTier();
	VRSTileSize = m_FeatureSupport.ShadingRateImageTileSize();
	MeshShaderSupport = m_FeatureSupport.MeshShaderTier();
	SamplerFeedbackSupport = m_FeatureSupport.SamplerFeedbackTier();
	ShaderModel = (uint16)m_FeatureSupport.HighestShaderModel();
}

bool GraphicsCapabilities::CheckUAVSupport(DXGI_FORMAT format) const
{
	switch (format)
	{
	case DXGI_FORMAT_R32_FLOAT:
	case DXGI_FORMAT_R32_UINT:
	case DXGI_FORMAT_R32_SINT:
		// Unconditionally supported.
		return true;

	case DXGI_FORMAT_R32G32B32A32_FLOAT:
	case DXGI_FORMAT_R32G32B32A32_UINT:
	case DXGI_FORMAT_R32G32B32A32_SINT:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R16G16B16A16_UINT:
	case DXGI_FORMAT_R16G16B16A16_SINT:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UINT:
	case DXGI_FORMAT_R8G8B8A8_SINT:
	case DXGI_FORMAT_R16_FLOAT:
	case DXGI_FORMAT_R16_UINT:
	case DXGI_FORMAT_R16_SINT:
	case DXGI_FORMAT_R8_UNORM:
	case DXGI_FORMAT_R8_UINT:
	case DXGI_FORMAT_R8_SINT:
		// All these are supported if this optional feature is set.
		return m_FeatureSupport.TypedUAVLoadAdditionalFormats();

	case DXGI_FORMAT_R16G16B16A16_UNORM:
	case DXGI_FORMAT_R16G16B16A16_SNORM:
	case DXGI_FORMAT_R32G32_FLOAT:
	case DXGI_FORMAT_R32G32_UINT:
	case DXGI_FORMAT_R32G32_SINT:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
	case DXGI_FORMAT_R10G10B10A2_UINT:
	case DXGI_FORMAT_R11G11B10_FLOAT:
	case DXGI_FORMAT_R8G8B8A8_SNORM:
	case DXGI_FORMAT_R16G16_FLOAT:
	case DXGI_FORMAT_R16G16_UNORM:
	case DXGI_FORMAT_R16G16_UINT:
	case DXGI_FORMAT_R16G16_SNORM:
	case DXGI_FORMAT_R16G16_SINT:
	case DXGI_FORMAT_R8G8_UNORM:
	case DXGI_FORMAT_R8G8_UINT:
	case DXGI_FORMAT_R8G8_SNORM:
	case DXGI_FORMAT_R8G8_SINT:
	case DXGI_FORMAT_R16_UNORM:
	case DXGI_FORMAT_R16_SNORM:
	case DXGI_FORMAT_R8_SNORM:
	case DXGI_FORMAT_A8_UNORM:
	case DXGI_FORMAT_B5G6R5_UNORM:
	case DXGI_FORMAT_B5G5R5A1_UNORM:
	case DXGI_FORMAT_B4G4R4A4_UNORM:
		
		// Conditionally supported by specific pDevices.
		if (m_FeatureSupport.TypedUAVLoadAdditionalFormats())
		{
			D3D12_FORMAT_SUPPORT1 f1 = D3D12_FORMAT_SUPPORT1_NONE;
			D3D12_FORMAT_SUPPORT2 f2 = D3D12_FORMAT_SUPPORT2_NONE;
			VERIFY_HR(m_FeatureSupport.FormatSupport(format, f1, f2));
			const DWORD mask = D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE;
			return ((f2 & mask) == mask);
		}
		return false;

	default:
		return false;
	}
}

SwapChain::SwapChain(GraphicsDevice* pDevice, IDXGIFactory6* pFactory, WindowHandle pNativeWindow, DXGI_FORMAT format, uint32 width, uint32 height, uint32 numFrames, bool vsync)
	: m_Format(format), m_CurrentImage(0), m_Vsync(vsync)
{
	DXGI_SWAP_CHAIN_DESC1 desc{};
	desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	desc.BufferCount = numFrames;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
	desc.Format = format;
	desc.Width = width;
	desc.Height = height;
	desc.Scaling = DXGI_SCALING_NONE;
	desc.Stereo = FALSE;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;

	DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsDesc{};
	fsDesc.RefreshRate.Denominator = 60;
	fsDesc.RefreshRate.Numerator = 1;
	fsDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
	fsDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
	fsDesc.Windowed = true;

	CommandQueue* pPresentQueue = pDevice->GetCommandQueue(D3D12_COMMAND_LIST_TYPE_DIRECT);
	ComPtr<IDXGISwapChain1> swapChain;
	
	VERIFY_HR(pFactory->CreateSwapChainForHwnd(
		pPresentQueue->GetCommandQueue(),
		(HWND)pNativeWindow,
		&desc,
		&fsDesc,
		nullptr,
		swapChain.GetAddressOf()));

	m_pSwapchain.Reset();
	swapChain.As(&m_pSwapchain);

	m_Backbuffers.resize(numFrames);
	for (uint32 i = 0; i < numFrames; ++i)
	{
		m_Backbuffers[i] = std::make_unique<Texture>(pDevice, "Render Target");
	}
}

SwapChain::~SwapChain()
{
	m_pSwapchain->SetFullscreenState(false, nullptr);
}

void SwapChain::OnResize(uint32 width, uint32 height)
{
	for (size_t i = 0; i < m_Backbuffers.size(); ++i)
	{
		m_Backbuffers[i]->Release();
	}

	//Resize the buffers
	DXGI_SWAP_CHAIN_DESC1 desc{};
	m_pSwapchain->GetDesc1(&desc);
	VERIFY_HR(m_pSwapchain->ResizeBuffers(
		(uint32)m_Backbuffers.size(),
		width,
		height,
		desc.Format,
		desc.Flags
	));

	m_CurrentImage = 0;

	//Recreate the render target views
	for (uint32 i = 0; i < (uint32)m_Backbuffers.size(); ++i)
	{
		ID3D12Resource* pResource = nullptr;
		VERIFY_HR(m_pSwapchain->GetBuffer(i, IID_PPV_ARGS(&pResource)));
		m_Backbuffers[i]->CreateForSwapchain(pResource);
	}
}

void SwapChain::Present()
{
	m_pSwapchain->Present(m_Vsync, m_Vsync ? 0 : DXGI_PRESENT_ALLOW_TEARING);
	m_CurrentImage = m_pSwapchain->GetCurrentBackBufferIndex();
}
