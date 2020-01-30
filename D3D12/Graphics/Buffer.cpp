#include "stdafx.h"
#include "Buffer.h"
#include "CommandContext.h"
#include "Graphics.h"
#include "Texture.h"


Buffer::Buffer(ID3D12Resource* pResource, D3D12_RESOURCE_STATES state)
	: GraphicsResource(pResource, state)
{

}

void Buffer::Create(Graphics* pGraphics, const BufferDesc& bufferDesc)
{
	Release();
	m_Desc = bufferDesc;

	D3D12_RESOURCE_DESC desc;
	desc.Alignment = 0;
	desc.DepthOrArraySize = 1;
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Flags = D3D12_RESOURCE_FLAG_NONE;
	desc.Format = DXGI_FORMAT_UNKNOWN;
	desc.Height = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	desc.MipLevels = 1;
	desc.Width = (int64)bufferDesc.ByteStride * bufferDesc.ElementCount;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;

	if ((bufferDesc.Usage & BufferUsage::ShaderResource) != BufferUsage::ShaderResource)
	{
		desc.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
	}
	if ((bufferDesc.Usage & BufferUsage::UnorderedAccess) == BufferUsage::UnorderedAccess)
	{
		desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		m_CurrentState = D3D12_RESOURCE_STATE_GENERIC_READ;
	}

	if (bufferDesc.Storage == BufferStorageType::Readback)
	{
		m_CurrentState = D3D12_RESOURCE_STATE_COPY_DEST;
	}

	D3D12_HEAP_TYPE heapType = (D3D12_HEAP_TYPE)0;
	switch (bufferDesc.Storage)
	{
	case BufferStorageType::Default:
		heapType = D3D12_HEAP_TYPE_DEFAULT;
		break;
	case BufferStorageType::Upload:
		heapType = D3D12_HEAP_TYPE_UPLOAD;
		break;
	case BufferStorageType::Readback:
		heapType = D3D12_HEAP_TYPE_READBACK;
		break;
	default:
		assert(false);
		break;
	}

	m_pResource = pGraphics->CreateResource(desc, m_CurrentState, heapType);
}

void Buffer::SetData(CommandContext* pContext, const void* pData, uint64 dataSize, uint32 offset)
{
	assert(dataSize + offset <= GetSize());
	pContext->InitializeBuffer(this, pData, dataSize);
}

void* Buffer::Map(uint32 subResource /*= 0*/, uint64 readFrom /*= 0*/, uint64 readTo /*= 0*/)
{
	assert(m_pResource);
	CD3DX12_RANGE range(readFrom, readTo);
	void* pMappedData = nullptr;
	m_pResource->Map(subResource, &range, &pMappedData);
	return pMappedData;
}

void Buffer::Unmap(uint32 subResource /*= 0*/, uint64 writtenFrom /*= 0*/, uint64 writtenTo /*= 0*/)
{
	assert(m_pResource);
	CD3DX12_RANGE range(writtenFrom, writtenFrom);
	m_pResource->Unmap(subResource, &range);
}

void BufferSRV::Create(Graphics* pGraphics, Buffer* pBuffer, const BufferSRVDesc& desc)
{
	assert(pBuffer);
	m_pParent = pBuffer;

	if (m_Descriptor.ptr == 0)
	{
		m_Descriptor = pGraphics->AllocateCpuDescriptors(1, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	}

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Format = desc.Format;
	srvDesc.Buffer.Flags = desc.IsRaw ? D3D12_BUFFER_SRV_FLAG_RAW : D3D12_BUFFER_SRV_FLAG_NONE;
	srvDesc.Buffer.NumElements = pBuffer->GetDesc().ElementCount;
	srvDesc.Buffer.StructureByteStride = desc.IsRaw ? 0 : pBuffer->GetDesc().ByteStride;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;

	pGraphics->GetDevice()->CreateShaderResourceView(pBuffer->GetResource(), &srvDesc, m_Descriptor);
}

void BufferUAV::Create(Graphics* pGraphics, Buffer* pBuffer, const BufferUAVDesc& desc)
{
	assert(pBuffer);
	m_pParent = pBuffer;

	if (m_Descriptor.ptr == 0)
	{
		m_Descriptor = pGraphics->AllocateCpuDescriptors(1, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	}

	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.Format = desc.Format;
	uavDesc.Buffer.Flags = desc.IsRaw ? D3D12_BUFFER_UAV_FLAG_RAW : D3D12_BUFFER_UAV_FLAG_NONE;
	uavDesc.Buffer.NumElements = pBuffer->GetDesc().ElementCount;
	uavDesc.Buffer.StructureByteStride = desc.IsRaw ? 0 : pBuffer->GetDesc().ByteStride;
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;

	pGraphics->GetDevice()->CreateUnorderedAccessView(pBuffer->GetResource(), desc.pCounter ? desc.pCounter->GetResource() : nullptr, &uavDesc, m_Descriptor);
}