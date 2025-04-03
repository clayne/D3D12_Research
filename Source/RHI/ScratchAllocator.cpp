#include "stdafx.h"
#include "ScratchAllocator.h"
#include "Device.h"
#include "Buffer.h"
#include "CommandContext.h"

ScratchAllocationManager::ScratchAllocationManager(GraphicsDevice* pParent, BufferFlag bufferFlags, uint64 pageSize)
	: DeviceObject(pParent), m_BufferFlags(bufferFlags), m_PageSize(pageSize)
{
}

Ref<Buffer> ScratchAllocationManager::AllocatePage()
{
	auto AllocateNewPage = [this]() {
		String name = Sprintf("Dynamic Allocation Buffer (%f KB)", Math::BytesToKiloBytes * m_PageSize);
		return GetParent()->CreateBuffer(BufferDesc{ .Size = m_PageSize, .Flags = BufferFlag::Upload }, "Page");
	};
	return m_PagePool.Allocate(AllocateNewPage);
}

void ScratchAllocationManager::FreePages(const SyncPoint& syncPoint, const Array<Ref<Buffer>>& pPages)
{
	for (auto pPage : pPages)
	{
		m_PagePool.Free(std::move(pPage), syncPoint);
	}
}

void ScratchAllocator::Init(ScratchAllocationManager* pPageManager)
{
	m_pPageManager = pPageManager;
}

ScratchAllocation ScratchAllocator::Allocate(uint64 size, int alignment)
{
	uint64 bufferSize = Math::AlignUp<uint64>(size, alignment);
	ScratchAllocation allocation;
	allocation.Size = size;

	if (bufferSize > m_pPageManager->GetPageSize())
	{
		Ref<Buffer> pPage = m_pPageManager->GetParent()->CreateBuffer(BufferDesc{ .Size = size, .Flags = BufferFlag::Upload }, "Large Page");
		allocation.Offset = 0;
		allocation.GPUAddress = pPage->GetGPUAddress();
		allocation.pBackingResource = pPage;
		allocation.pMappedMemory = pPage->GetMappedData();
	}
	else
	{
		m_CurrentOffset = Math::AlignUp<uint64>(m_CurrentOffset, alignment);

		if (m_pCurrentPage == nullptr || m_CurrentOffset + bufferSize >= m_pCurrentPage->GetSize())
		{
			m_pCurrentPage = m_pPageManager->AllocatePage();
			m_CurrentOffset = 0;
			m_UsedPages.push_back(m_pCurrentPage);
		}
		allocation.Offset = m_CurrentOffset;
		allocation.GPUAddress = m_pCurrentPage->GetGPUAddress() + m_CurrentOffset;
		allocation.pBackingResource = m_pCurrentPage;
		allocation.pMappedMemory = (char*)m_pCurrentPage->GetMappedData() + m_CurrentOffset;

		m_CurrentOffset += bufferSize;
	}
	return allocation;
}

void ScratchAllocator::Free(const SyncPoint& syncPoint)
{
	m_pPageManager->FreePages(syncPoint, m_UsedPages);
	m_UsedPages.clear();

	m_pCurrentPage = nullptr;
	m_CurrentOffset = 0;
}
