#include "stdafx.h"
#include "CommandContext.h"
#include "Graphics.h"
#include "CommandQueue.h"
#include "DynamicResourceAllocator.h"
#include "GraphicsResource.h"

#if _DEBUG
#include <pix3.h>
#endif
#include "DynamicDescriptorAllocator.h"
#include "PipelineState.h"
#include "RootSignature.h"

CommandContext::CommandContext(Graphics* pGraphics, ID3D12GraphicsCommandList* pCommandList, ID3D12CommandAllocator* pAllocator, D3D12_COMMAND_LIST_TYPE type)
	: m_pGraphics(pGraphics), m_pCommandList(pCommandList), m_pAllocator(pAllocator), m_Type(type)
{
	m_pDynamicDescriptorAllocator = std::make_unique<DynamicDescriptorAllocator>(pGraphics, this, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

CommandContext::~CommandContext()
{
}

void CommandContext::Reset()
{
	m_pAllocator = m_pGraphics->GetCommandQueue(m_Type)->RequestAllocator();
	m_pCommandList->Reset(m_pAllocator, nullptr);
	BindDescriptorHeaps();
}

uint64 CommandContext::Execute(bool wait)
{
	FlushResourceBarriers();
	CommandQueue* pQueue = m_pGraphics->GetCommandQueue(m_Type);
	uint64 fenceValue = pQueue->ExecuteCommandList(m_pCommandList);
	pQueue->FreeAllocator(fenceValue, m_pAllocator);
	if (wait)
	{
		pQueue->WaitForFence(fenceValue);
	}
	m_pGraphics->FreeCommandList(this);
	m_pGraphics->GetCpuVisibleAllocator()->Free(fenceValue);
	m_pDynamicDescriptorAllocator->ReleaseUsedHeaps(fenceValue);
	return fenceValue;
}

uint64 CommandContext::ExecuteAndReset(bool wait)
{
	FlushResourceBarriers();
	CommandQueue* pQueue = m_pGraphics->GetCommandQueue(m_Type);
	uint64 fenceValue = pQueue->ExecuteCommandList(m_pCommandList);
	if (wait)
	{
		pQueue->WaitForFence(fenceValue);
	}
	m_pCommandList->Reset(m_pAllocator, nullptr);
	return fenceValue;
}

void CommandContext::Draw(int vertexStart, int vertexCount)
{
	PrepareDraw();
	m_pCommandList->DrawInstanced(vertexCount, 1, vertexStart, 0);
}

void CommandContext::DrawIndexed(int indexCount, int indexStart, int minVertex /*= 0*/)
{
	PrepareDraw();
	m_pCommandList->DrawIndexedInstanced(indexCount, 1, indexStart, minVertex, 0);
}

void CommandContext::DrawIndexedInstanced(int indexCount, int indexStart, int instanceCount, int minVertex /*= 0*/, int instanceStart /*= 0*/)
{
	PrepareDraw();
	m_pCommandList->DrawIndexedInstanced(indexCount, instanceCount, indexStart, minVertex, instanceStart);
}

void CommandContext::ClearRenderTarget(D3D12_CPU_DESCRIPTOR_HANDLE rtv, const Color& color /*= Color(0.15f, 0.15f, 0.15f, 1.0f)*/)
{
	m_pCommandList->ClearRenderTargetView(rtv, &color.x, 0, nullptr);
}

void CommandContext::ClearDepth(D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS clearFlags /*= D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL*/, float depth /*= 1.0f*/, unsigned char stencil /*= 0*/)
{
	m_pCommandList->ClearDepthStencilView(dsv, clearFlags, depth, stencil, 0, nullptr);
}

void CommandContext::SetRenderTarget(D3D12_CPU_DESCRIPTOR_HANDLE* pRtv)
{
	m_pRenderTarget = pRtv;
}

void CommandContext::SetDepthStencil(D3D12_CPU_DESCRIPTOR_HANDLE* pDsv)
{
	m_pDepthStencilView = pDsv;
}

void CommandContext::SetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY type)
{
	m_pCommandList->IASetPrimitiveTopology(type);
}

void CommandContext::SetVertexBuffer(D3D12_VERTEX_BUFFER_VIEW vertexBufferView)
{
	SetVertexBuffers(&vertexBufferView, 1);
}

void CommandContext::SetVertexBuffers(D3D12_VERTEX_BUFFER_VIEW* pBuffers, int bufferCount)
{
	m_pCommandList->IASetVertexBuffers(0, bufferCount, pBuffers);
}

void CommandContext::SetIndexBuffer(D3D12_INDEX_BUFFER_VIEW indexBufferView)
{
	m_pCommandList->IASetIndexBuffer(&indexBufferView);
}

void CommandContext::SetViewport(const FloatRect& rect, float minDepth /*= 0.0f*/, float maxDepth /*= 1.0f*/)
{
	D3D12_VIEWPORT viewport;
	viewport.TopLeftX = (float)rect.Left;
	viewport.TopLeftY = (float)rect.Top;
	viewport.Height = (float)rect.GetHeight();
	viewport.Width = (float)rect.GetWidth();
	viewport.MinDepth = minDepth;
	viewport.MaxDepth = maxDepth;
	m_pCommandList->RSSetViewports(1, &viewport);
}

void CommandContext::SetScissorRect(const FloatRect& rect)
{
	D3D12_RECT r;
	r.left = (LONG)rect.Left;
	r.top = (LONG)rect.Top;
	r.right = (LONG)rect.Right;
	r.bottom = (LONG)rect.Bottom;
	m_pCommandList->RSSetScissorRects(1, &r);
}

void CommandContext::SetGraphicsRootSignature(RootSignature* pRootSignature)
{
	m_pCommandList->SetGraphicsRootSignature(pRootSignature->GetRootSignature());
	m_pDynamicDescriptorAllocator->ParseRootSignature(pRootSignature);
}

void CommandContext::SetPipelineState(PipelineState* pPipelineState)
{
	m_pCommandList->SetPipelineState(pPipelineState->GetPipelineState());
}

void CommandContext::FlushResourceBarriers()
{
	if (m_NumQueuedBarriers > 0)
	{
		m_pCommandList->ResourceBarrier(m_NumQueuedBarriers, m_QueuedBarriers.data());
		m_NumQueuedBarriers = 0;
	}
}

void CommandContext::SetDynamicConstantBufferView(int rootIndex, void* pData, uint32 dataSize)
{
	DynamicAllocation allocation = AllocatorUploadMemory(dataSize);
	memcpy(allocation.pMappedMemory, pData, dataSize);
	m_pCommandList->SetGraphicsRootConstantBufferView(rootIndex, allocation.GpuHandle);
}

void CommandContext::SetDynamicVertexBuffer(int rootIndex, int elementCount, int elementSize, void* pData)
{
	int bufferSize = elementCount * elementSize;
	DynamicAllocation allocation = AllocatorUploadMemory(bufferSize);
	memcpy(allocation.pMappedMemory, pData, elementCount * elementSize);
	D3D12_VERTEX_BUFFER_VIEW view = {};
	view.BufferLocation = allocation.GpuHandle;
	view.SizeInBytes = elementSize * elementCount;
	view.StrideInBytes = elementSize;
	m_pCommandList->IASetVertexBuffers(rootIndex, 1, &view);
}

void CommandContext::SetDynamicIndexBuffer(int elementCount, void* pData)
{
	int bufferSize = elementCount * sizeof(uint32);
	DynamicAllocation allocation = AllocatorUploadMemory(bufferSize);
	memcpy(allocation.pMappedMemory, pData, elementCount * sizeof(uint32));
	D3D12_INDEX_BUFFER_VIEW view = {};
	view.BufferLocation = allocation.GpuHandle;
	view.SizeInBytes = elementCount * sizeof(uint32);
	view.Format = DXGI_FORMAT_R32_UINT;
	m_pCommandList->IASetIndexBuffer(&view);
}

void CommandContext::SetDynamicDescriptor(int rootIndex, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	m_pDynamicDescriptorAllocator->SetDescriptors(rootIndex, 0, 1, &handle);
}

void CommandContext::SetDescriptorHeap(ID3D12DescriptorHeap* pHeap, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
	if (m_CurrentDescriptorHeaps[(int)type] != pHeap)
	{
		m_CurrentDescriptorHeaps[(int)type] = pHeap;
		BindDescriptorHeaps();
	}
}

DynamicAllocation CommandContext::AllocatorUploadMemory(uint32 size)
{
	return m_pGraphics->GetCpuVisibleAllocator()->Allocate(size);
}

void CommandContext::InitializeBuffer(GraphicsBuffer* pResource, void* pData, uint32 dataSize)
{
	DynamicAllocation allocation = AllocatorUploadMemory(dataSize);
	memcpy(allocation.pMappedMemory, pData, dataSize);
	InsertResourceBarrier(pResource, D3D12_RESOURCE_STATE_COPY_DEST, true);
	m_pCommandList->CopyBufferRegion(pResource->GetResource(), 0, allocation.pBackingResource, allocation.Offset, dataSize);
	InsertResourceBarrier(pResource, D3D12_RESOURCE_STATE_GENERIC_READ, true);
}

void CommandContext::InitializeTexture(Texture2D* pResource, void* pData, uint32 dataSize)
{
	DynamicAllocation allocation = m_pGraphics->GetCpuVisibleAllocator()->Allocate(dataSize, 512);
	memcpy(allocation.pMappedMemory, pData, dataSize);
	InsertResourceBarrier(pResource, D3D12_RESOURCE_STATE_COPY_DEST, true);

	D3D12_TEXTURE_COPY_LOCATION location = {};
	location.pResource = pResource->GetResource();
	location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	location.SubresourceIndex = 0;

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
	D3D12_RESOURCE_DESC desc = pResource->GetResource()->GetDesc();
	m_pGraphics->GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &layout, nullptr, nullptr, nullptr);
	layout.Offset = allocation.Offset;

	CD3DX12_TEXTURE_COPY_LOCATION sourceLocation = CD3DX12_TEXTURE_COPY_LOCATION(pResource->GetResource());
	CD3DX12_TEXTURE_COPY_LOCATION targetLocation = CD3DX12_TEXTURE_COPY_LOCATION(allocation.pBackingResource, layout);
	m_pCommandList->CopyTextureRegion(&sourceLocation, 0, 0, 0, &targetLocation, nullptr);

	InsertResourceBarrier(pResource, D3D12_RESOURCE_STATE_GENERIC_READ, true);
}

void CommandContext::InsertResourceBarrier(GraphicsResource* pBuffer, D3D12_RESOURCE_STATES state, bool executeImmediate /*= false*/)
{
	if (pBuffer->m_CurrentState != state)
	{
		m_QueuedBarriers[m_NumQueuedBarriers] = CD3DX12_RESOURCE_BARRIER::Transition(pBuffer->GetResource(), pBuffer->m_CurrentState, state);
		++m_NumQueuedBarriers;
		if (executeImmediate || m_NumQueuedBarriers >= m_QueuedBarriers.size())
		{
			FlushResourceBarriers();
		}
		pBuffer->m_CurrentState = state;
	}
}

void CommandContext::BindDescriptorHeaps()
{
	std::array<ID3D12DescriptorHeap*, D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES> heapsToBind = {};
	int heapCount = 0;
	for (size_t i = 0; i < heapsToBind.size(); ++i)
	{
		if (m_CurrentDescriptorHeaps[i] != nullptr)
		{
			heapsToBind[heapCount] = m_CurrentDescriptorHeaps[i];
			++heapCount;
		}
	}
	if (heapCount > 0)
	{
		m_pCommandList->SetDescriptorHeaps(heapCount, heapsToBind.data());
	}
}

void CommandContext::PrepareDraw()
{
	FlushResourceBarriers();
	m_pDynamicDescriptorAllocator->UploadAndBindStagedDescriptors();
	m_pCommandList->OMSetRenderTargets(1, m_pRenderTarget, false, m_pDepthStencilView);
}

void CommandContext::MarkBegin(const wchar_t* pName)
{
#ifdef _DEBUG
	::PIXBeginEvent(m_pCommandList, 0, pName);
#endif
}

void CommandContext::MarkEvent(const wchar_t* pName)
{
#ifdef _DEBUG
	::PIXSetMarker(m_pCommandList, 0, pName);
#endif
}

void CommandContext::MarkEnd()
{
#ifdef _DEBUG
	::PIXEndEvent(m_pCommandList);
#endif
}