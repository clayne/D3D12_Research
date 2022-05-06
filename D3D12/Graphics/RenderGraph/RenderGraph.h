#pragma once
#include "RenderGraphDefinitions.h"
#include "Graphics/RHI/Texture.h"
#include "Graphics/RHI/Buffer.h"
#include "Graphics/RHI/Fence.h"
#include "Graphics/RHI/CommandContext.h"
#include "Blackboard.h"

#define RG_GRAPH_SCOPE(name, graph) RGGraphScope MACRO_CONCAT(rgScope_,__COUNTER__)(name, graph)

class RGGraph;
class RGPass;

// Flags assigned to a pass that can determine various things
enum class RGPassFlag
{
	None =		0,
	// Raster pass
	Raster =	1 << 0,
	// Compute pass
	Compute =	1 << 1,
	// Pass that performs a copy resource operation. Does not play well with Raster/Compute passes
	Copy =		1 << 2,
	// Makes the pass invisible to profiling. Useful for adding debug markers
	Invisible = 1 << 3,
	// Makes a pass never be culled when not referenced.
	NeverCull = 1 << 4,
};
DECLARE_BITMASK_TYPE(RGPassFlag);

enum class RGResourceType
{
	Texture,
	Buffer,
};
template<typename T> struct RGResourceTypeTraits { };
template<> struct RGResourceTypeTraits<Texture> { constexpr static RGResourceType Type = RGResourceType::Texture; };
template<> struct RGResourceTypeTraits<Buffer> { constexpr static RGResourceType Type = RGResourceType::Buffer; };

enum class RGResourceAccess
{
	None =			0,
	SRV =			1 << 0,
	UAV =			1 << 1,
	RenderTarget =	1 << 2,
	Depth =			1 << 3,
};
DECLARE_BITMASK_TYPE(RGResourceAccess);

struct RGResource
{
	RGResource(const char* pName, int id, const TextureDesc& desc, Texture* pResource = nullptr)
		: Name(pName), Id(id), IsImported(!!pResource), Type(RGResourceType::Texture), pPhysicalResource(pResource), TextureDesc(desc)
	{}

	RGResource(const char* pName, int id, const BufferDesc& desc, Buffer* pResource = nullptr)
		: Name(pName), Id(id), IsImported(!!pResource), Type(RGResourceType::Buffer), pPhysicalResource(pResource), BufferDesc(desc)
	{}

	const char* Name;
	int Id;
	bool IsImported;
	int Version = 0;
	RGResourceType Type;
	RefCountPtr<GraphicsResource> pPhysicalResource;

	template<typename T>
	T* GetRHI() const
	{
		RG_ASSERT(Type == RGResourceTypeTraits<T>::Type, "Provided type does not match resource type");
		return static_cast<T*>(pPhysicalResource.Get());
	}

	union
	{
		TextureDesc TextureDesc;
		BufferDesc BufferDesc;
	};

	//RenderGraph compile-time values
	int References = 0;
	RGPass* pFirstAccess = nullptr;
	RGPass* pLastAccess = nullptr;
};

struct RGNode
{
	explicit RGNode(RGResource* pResource)
		: pResource(pResource), Version(pResource->Version)
	{}

	RGResource* pResource;
	int Version;
	RGResourceAccess UseFlags = RGResourceAccess::None;
	//RenderGraph compile-time values
	RGPass* pWriter = nullptr;
	int Reads = 0;
};

class RGPassResources
{
public:
	RGPassResources(RGGraph& graph, RGPass& pass)
		: m_Graph(graph), m_Pass(pass)
	{}

	RGPassResources(const RGPassResources& other) = delete;
	RGPassResources& operator=(const RGPassResources& other) = delete;

	template<typename T>
	T* Get(RGResourceHandle handle) const
	{
		return GetResource(handle)->GetRHI<T>();
	}

	RenderPassInfo GetRenderPassInfo() const;

private:
	const RGResource* GetResource(RGResourceHandle handle) const;

	RGGraph& m_Graph;
	RGPass& m_Pass;
};

class RGPass
{
public:
	friend class RGGraph;
	friend class RGPassResources;

	struct RenderTargetAccess
	{
		RGResourceHandle Resource;
		RenderPassAccess Access;
	};

	struct DepthStencilAccess
	{
		RGResourceHandle Resource;
		RenderPassAccess Access;
		RenderPassAccess StencilAccess;
		bool Write;
	};

private:
	RGPass(RGGraph& graph, const char* pName, RGPassFlag flags, int id)
		: Graph(graph), Flags(flags), ID(id)
	{
		strcpy_s(Name, pName);
	}

	RGPass(const RGPass& rhs) = delete;
	RGPass& operator=(const RGPass& rhs) = delete;

public:
	template<typename ExecuteFn>
	RGPass& Bind(ExecuteFn&& callback)
	{
		RG_STATIC_ASSERT(sizeof(ExecuteFn) < 4096, "The Execute callback exceeds the maximum size");
		checkf(!ExecuteCallback.IsBound(), "Pass is already bound! This may be unintentional");
		ExecuteCallback.BindLambda(std::move(callback));
		return *this;
	}

	RGPass& Write(Span<RGResourceHandle*> resources);
	RGPass& Read(Span<RGResourceHandle> resources);
	RGPass& ReadWrite(Span<RGResourceHandle*> resources);
	RGPass& RenderTarget(RGResourceHandle& resource, RenderPassAccess access);
	RGPass& DepthStencil(RGResourceHandle& resource, RenderPassAccess depthAccess, bool write, RenderPassAccess stencilAccess = RenderPassAccess::NoAccess);

private:

	void Read(Span<RGResourceHandle> resources, RGResourceAccess useFlag);
	void Write(Span<RGResourceHandle*> resources, RGResourceAccess useFlag);

	bool ReadsFrom(RGResourceHandle handle) const
	{
		return std::find(Reads.begin(), Reads.end(), handle) != Reads.end();
	}

	bool WritesTo(RGResourceHandle handle) const
	{
		return std::find(Writes.begin(), Writes.end(), handle) != Writes.end();
	}

	DECLARE_DELEGATE(ExecutePassDelegate, CommandContext& /*context*/, const RGPassResources& /*resources*/);
	char Name[128];
	RGGraph& Graph;
	int ID;
	RGPassFlag Flags;
	std::vector<RGResourceHandle> Reads;
	std::vector<RGResourceHandle> Writes;
	std::vector<RenderTargetAccess> RenderTargets;
	DepthStencilAccess DepthStencilTarget;
	ExecutePassDelegate ExecuteCallback;

	//RenderGraph compile-time values
	int References = 0;
};

class TexturePool : public GraphicsObject
{
public:
	TexturePool(GraphicsDevice* pDevice)
		: GraphicsObject(pDevice)
	{}

	RefCountPtr<Texture> Allocate(const char* pName, const TextureDesc& desc);
	void Tick();

private:
	struct PooledTexture
	{
		RefCountPtr<Texture> pTexture;
		uint32 LastUsedFrame;
	};
	std::vector<PooledTexture> m_TexturePool;
	uint32 m_FrameIndex = 0;
};

class RGGraph
{
	class Allocator
	{
	public:
		struct AllocatedObject
		{
			virtual ~AllocatedObject() = default;
		};

		template<typename T>
		struct TAllocatedObject : public AllocatedObject
		{
			template<typename... Args>
			TAllocatedObject(Args... args)
				: Object(std::forward<Args>(args)...)
			{}
			T Object;
		};

		Allocator(uint64 size)
			: m_Size(size), m_pData(new char[size]), m_pCurrentOffset(m_pData)
		{}

		~Allocator()
		{
			for(size_t i = 0; i < m_NonPODAllocations.size(); ++i)
			{
				m_NonPODAllocations[i]->~AllocatedObject();
			}
			delete[] m_pData;
		}

		template<typename T, typename ...Args>
		T* Allocate(Args... args)
		{
			using AllocatedType = std::conditional_t<std::is_pod_v<T>, T, TAllocatedObject<T>>;
			check(m_pCurrentOffset - m_pData + sizeof(AllocatedType) < m_Size);
			void* pData = m_pCurrentOffset;
			m_pCurrentOffset += sizeof(AllocatedType);
			AllocatedType* pAllocation = new (pData) AllocatedType(std::forward<Args>(args)...);

			if constexpr (std::is_pod_v<T>)
			{
				return pAllocation;
			}
			else
			{
				m_NonPODAllocations.push_back(pAllocation);
				return &pAllocation->Object;
			}
		}

	private:
		std::vector<AllocatedObject*> m_NonPODAllocations;
		uint64 m_Size;
		char* m_pData;
		char* m_pCurrentOffset;
	};

public:
	RGGraph(GraphicsDevice* pDevice, TexturePool& texturePool, uint64 allocatorSize = 0xFFFF);
	~RGGraph();

	RGGraph(const RGGraph& other) = delete;
	RGGraph& operator=(const RGGraph& other) = delete;

	void Compile();
	SyncPoint Execute();
	void DumpGraph(const char* pPath) const;
	RGResourceHandle MoveResource(RGResourceHandle From, RGResourceHandle To);

	RGPass& AddCopyTexturePass(const char* pName, RGResourceHandle source, RGResourceHandle& target);

	template<typename T, typename... Args>
	T* Allocate(Args... args)
	{
		return m_Allocator.Allocate<T>(std::forward<Args>(args)...);
	}

	RGPass& AddPass(const char* pName, RGPassFlag flags)
	{
		RGPass* pPass = Allocate<RGPass>(std::ref(*this), pName, flags, (int)m_RenderPasses.size());
		m_RenderPasses.push_back(pPass);
		return *m_RenderPasses.back();
	}

	RGResourceHandle CreateTexture(const char* pName, const TextureDesc& desc)
	{
		RGResource* pResource = Allocate<RGResource>(pName, (int)m_Resources.size(), desc);
		m_Resources.push_back(pResource);
		return CreateResourceNode(pResource);
	}

	RGResourceHandle CreateBuffer(const char* pName, const BufferDesc& desc)
	{
		RGResource* pResource = Allocate<RGResource>(pName, (int)m_Resources.size(), desc);
		m_Resources.push_back(pResource);
		return CreateResourceNode(pResource);
	}

	RGResourceHandle ImportTexture(const char* pName, Texture* pTexture)
	{
		check(pTexture);
		RGResource* pResource = Allocate<RGResource>(pName, (int)m_Resources.size(), pTexture->GetDesc(), pTexture);
		m_Resources.push_back(pResource);
		return CreateResourceNode(pResource);
	}

	RGResourceHandle ImportBuffer(const char* pName, Buffer* pBuffer)
	{
		check(pBuffer);
		RGResource* pResource = Allocate<RGResource>(pName, (int)m_Resources.size(), pBuffer->GetDesc(), pBuffer);
		m_Resources.push_back(pResource);
		return CreateResourceNode(pResource);
	}

	bool IsValidHandle(RGResourceHandle handle) const
	{
		return handle.IsValid() && handle.Index < (int)m_ResourceNodes.size();
	}

	RGResourceHandle CreateResourceNode(RGResource* pResource)
	{
		RGNode node(pResource);
		m_ResourceNodes.push_back(node);
		return RGResourceHandle((int)m_ResourceNodes.size() - 1);
	}

	const RGNode& GetResourceNode(RGResourceHandle handle) const
	{
		RG_ASSERT(IsValidHandle(handle), "Invalid handle");
		return m_ResourceNodes[handle.Index];
	}

	RGNode& GetResourceNode(RGResourceHandle handle)
	{
		RG_ASSERT(IsValidHandle(handle), "Invalid handle");
		return m_ResourceNodes[handle.Index];
	}

	RGResource* GetResource(RGResourceHandle handle) const
	{
		const RGNode& node = GetResourceNode(handle);
		return node.pResource;
	}

	const TextureDesc& GetDesc(RGResourceHandle handle) const
	{
		RGResource* pResource = GetResource(handle);
		check(pResource->Type == RGResourceType::Texture);
		return pResource->TextureDesc;
	}

	RGBlackboard& Blackboard() { return m_Blackboard; }

	void PushEvent(const char* pName);
	void PopEvent();

private:
	void ExecutePass(RGPass* pPass, CommandContext& context);
	void PrepareResources(RGPass* pPass, CommandContext& context);
	void ReleaseResources(RGPass* pPass);
	void DestroyData();

	struct RGResourceAlias
	{
		RGResourceHandle From;
		RGResourceHandle To;
	};

	GraphicsDevice* m_pDevice;
	Allocator m_Allocator;
	SyncPoint m_LastSyncPoint;

	RGBlackboard m_Blackboard;
	std::vector<RGResourceAlias> m_Aliases;
	std::vector<RGPass*> m_RenderPasses;
	std::vector<RGResource*> m_Resources;
	std::vector<RGNode> m_ResourceNodes;
	TexturePool& m_TexturePool;
};

class RGGraphScope
{
public:
	RGGraphScope(const char* pName, RGGraph& graph)
		: m_Graph(graph)
	{
		graph.PushEvent(pName);
	}
	~RGGraphScope()
	{
		m_Graph.PopEvent();
	}
private:
	RGGraph& m_Graph;
};
