
#include "stdafx.h"
#include "Profiler.h"

#if WITH_PROFILING

CPUProfiler gCPUProfiler;
GPUProfiler gGPUProfiler;

static uint32 ColorFromString(const char* pStr, float hueMin, float hueMax)
{
	const float saturation = 0.5f;
	const float value = 0.6f;
	float hue = (float)std::hash<std::string>{}(pStr) / std::numeric_limits<size_t>::max();
	hue = hueMin + hue * (hueMax - hueMin);
	float R = std::max(std::min(fabs(hue * 6 - 3) - 1, 1.0f), 0.0f);
	float G = std::max(std::min(2 - fabs(hue * 6 - 2), 1.0f), 0.0f);
	float B = std::max(std::min(2 - fabs(hue * 6 - 4), 1.0f), 0.0f);

	R = ((R - 1) * saturation + 1) * value;
	G = ((G - 1) * saturation + 1) * value;
	B = ((B - 1) * saturation + 1) * value;

	return
		((uint8)roundf(R * 255.0f) << 0) |
		((uint8)roundf(G * 255.0f) << 8) |
		((uint8)roundf(B * 255.0f) << 16);
}


//-----------------------------------------------------------------------------
// [SECTION] GPU Profiler
//-----------------------------------------------------------------------------

void GPUProfiler::Initialize(
	ID3D12Device*				pDevice,
	Span<ID3D12CommandQueue*>	queues,
	uint32						sampleHistory,
	uint32						frameLatency,
	uint32						maxNumEvents,
	uint32						maxNumCopyEvents,
	uint32						maxNumActiveCommandLists)
{
	// Event indices are 16 bit, so max 2^16 events
	gAssert(maxNumEvents + maxNumCopyEvents < (1u << 16u));

	m_FrameLatency		= frameLatency;
	m_EventHistorySize	= sampleHistory;

	InitializeSRWLock(&m_CommandListMapLock);
	m_CommandListData.resize(maxNumActiveCommandLists);

	m_QueueEventStack.resize(queues.GetSize());
	for (uint32 queueIndex = 0; queueIndex < queues.GetSize(); ++queueIndex)
	{
		ID3D12CommandQueue* pQueue = queues[queueIndex];
		D3D12_COMMAND_QUEUE_DESC desc = pQueue->GetDesc();

		m_QueueIndexMap[pQueue] = (uint32)m_Queues.size();
		QueueInfo& queueInfo = m_Queues.emplace_back();
		uint32 size = ARRAYSIZE(queueInfo.Name);
		if(FAILED(pQueue->GetPrivateData(WKPDID_D3DDebugObjectName, &size, queueInfo.Name)))
		{
			switch (desc.Type)
			{
			case D3D12_COMMAND_LIST_TYPE_DIRECT:		strcpy_s(queueInfo.Name, "Direct Queue");			break;
			case D3D12_COMMAND_LIST_TYPE_COMPUTE:		strcpy_s(queueInfo.Name, "Compute Queue");			break;
			case D3D12_COMMAND_LIST_TYPE_COPY:			strcpy_s(queueInfo.Name, "Copy Queue");				break;
			case D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE:	strcpy_s(queueInfo.Name, "Video Decode Queue");		break;
			case D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE:	strcpy_s(queueInfo.Name, "Video Encode Queue");		break;
			case D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS:	strcpy_s(queueInfo.Name, "Video Process Queue");	break;
			default:									strcpy_s(queueInfo.Name, "Unknown Queue");			break;
			}
		}

		queueInfo.pQueue			= pQueue;
		queueInfo.Index				= queueIndex;
		queueInfo.QueryHeapIndex	= desc.Type == D3D12_COMMAND_LIST_TYPE_COPY ? 1 : 0;
		pQueue->GetClockCalibration(&queueInfo.GPUCalibrationTicks, &queueInfo.CPUCalibrationTicks);
		pQueue->GetTimestampFrequency(&queueInfo.GPUFrequency);

		if (!GetHeap(desc.Type).IsInitialized())
			GetHeap(desc.Type).Initialize(pDevice, pQueue, 2 * (desc.Type == D3D12_COMMAND_LIST_TYPE_COPY ? maxNumCopyEvents : maxNumEvents), frameLatency);
	}
	QueryPerformanceFrequency((LARGE_INTEGER*)&m_CPUTickFrequency);

	m_pEventData = new ProfilerEventData[sampleHistory];
	for (uint32 i = 0; i < sampleHistory; ++i)
	{
		ProfilerEventData& eventData = m_pEventData[i];
		eventData.Events.resize(maxNumEvents + maxNumCopyEvents);
		eventData.EventOffsetAndCountPerTrack.resize(queues.GetSize());
	}

	m_pQueryData = new QueryData[frameLatency];

	m_IsInitialized = true;
}

void GPUProfiler::Shutdown()
{
	delete[] m_pEventData;
	delete[] m_pQueryData;

	for (QueryHeap& heap : m_QueryHeaps)
		heap.Shutdown();
}

void GPUProfiler::BeginEvent(ID3D12GraphicsCommandList* pCmd, const char* pName, uint32 color, const char* pFilePath, uint32 lineNumber)
{
	if (!m_IsInitialized)
		return;

	if (m_EventCallback.OnEventBegin)
		m_EventCallback.OnEventBegin(pName, pCmd, m_EventCallback.pUserData);

	if (m_IsPaused)
		return;

	ProfilerEventData& eventData	= GetSampleFrame();

	// Register a query on the commandlist
	CommandListState& state = *GetState(pCmd, true);
	CommandListState::Query& cmdListQuery = state.Queries.emplace_back();

	// Allocate a query range. This stores a begin/end query index pair. (Also event index)
	uint32 eventIndex = m_EventIndex.fetch_add(1);
	if (eventIndex >= eventData.Events.size())
		return;

	// Record a timestamp query and assign to the commandlist
	cmdListQuery.QueryIndex			= GetHeap(pCmd->GetType()).RecordQuery(pCmd);
	cmdListQuery.EventIndex			= eventIndex;

	// Allocate an event in the sample history
	ProfilerEvent& event			= eventData.Events[eventIndex];
	event.pName						= eventData.Allocator.String(pName);
	event.pFilePath					= pFilePath;
	event.LineNumber				= lineNumber;
	event.Color						= color == 0 ? ColorFromString(pName, 0.0f, 0.5f) : color;
}


void GPUProfiler::EndEvent(ID3D12GraphicsCommandList* pCmd)
{
	if (!m_IsInitialized)
		return;

	if (m_EventCallback.OnEventEnd)
		m_EventCallback.OnEventEnd(pCmd, m_EventCallback.pUserData);

	if (m_IsPaused)
		return;

	// Record a timestamp query and assign to the commandlist
	CommandListState& state			= *GetState(pCmd, true);
	CommandListState::Query& query	= state.Queries.emplace_back();
	query.QueryIndex				= GetHeap(pCmd->GetType()).RecordQuery(pCmd);
	query.EventIndex				= CommandListState::Query::EndEventFlag; // Range index to indicate this is an 'End' query
}

void GPUProfiler::Tick()
{
	if (!m_IsInitialized)
		return;

	for (ActiveEventStack& stack : m_QueueEventStack)
		gAssert(stack.GetSize() == 0, "EventStack for the CommandQueue should be empty. Forgot to `End()` %d Events", stack.GetSize());

	// If the next frame is not finished resolving, wait for it here so the data can be read from before it's being reset
	for (QueryHeap& heap : m_QueryHeaps)
		heap.WaitFrame(m_FrameIndex);

	ProfilerEventData& currEventFrame = GetSampleFrame(m_FrameIndex);
	currEventFrame.NumEvents = Math::Min((uint32)currEventFrame.Events.size(), (uint32)m_EventIndex);
	m_EventIndex = 0;

	// Poll query heap and populate event timings
	while (m_FrameToReadback < m_FrameIndex)
	{
		// Wait for all query heaps to have finished resolving the queries for the readback frame
		bool allHeapsReady = true;
		for (QueryHeap& heap : m_QueryHeaps)
			allHeapsReady &= heap.IsFrameComplete(m_FrameToReadback);
		if (!allHeapsReady)
			break;

		std::scoped_lock lock(m_QueryRangeLock);

		QueryData& queryData			= GetQueryData(m_FrameToReadback);
		ProfilerEventData& eventData	= GetSampleFrame(m_FrameToReadback);

		for (uint32 i = 0; i < eventData.NumEvents; ++i)
		{
			ProfilerEvent& event				= eventData.Events[i];
			QueryData::QueryPair& queryRange	= queryData.Pairs[i];
			if (!queryRange.IsValid())
			{
				event.TicksBegin = 0;
				event.TicksEnd = 0;
				continue;
			}

			const QueueInfo&	queue	= m_Queues[event.QueueIndex];
			Span<const uint64>	queries	= m_QueryHeaps[queue.QueryHeapIndex].GetQueryData(m_FrameToReadback);

			// Convert to CPU ticks and assign to event
			event.TicksBegin = ConvertToCPUTicks(queue, queries[queryRange.QueryIndexBegin]);
			event.TicksEnd = ConvertToCPUTicks(queue, queries[queryRange.QueryIndexEnd]);

		}

		// Sort events by queue and make groups per queue for fast per-queue event iteration.
		// This is _much_ faster than iterating all event multiple times and filtering
		Array<ProfilerEvent>& events = eventData.Events;
		std::sort(events.begin(), events.begin() + eventData.NumEvents, [](const ProfilerEvent& a, const ProfilerEvent& b)
			{
				return a.QueueIndex < b.QueueIndex;
			});

		URange eventRange(0, 0);
		for (uint32 queueIndex = 0; queueIndex < (uint32)m_Queues.size() && eventRange.Begin < eventData.NumEvents; ++queueIndex)
		{
			while (queueIndex > events[eventRange.Begin].QueueIndex)
				eventRange.Begin++;
			eventRange.End = eventRange.Begin;
			while (events[eventRange.End].QueueIndex == queueIndex && eventRange.End < eventData.NumEvents)
				++eventRange.End;

			eventData.EventOffsetAndCountPerTrack[queueIndex] = ProfilerEventData::OffsetAndSize(eventRange.Begin, eventRange.End - eventRange.Begin);
			eventRange.Begin = eventRange.End;
		}

		++m_FrameToReadback;
	}

	m_IsPaused = m_PauseQueued;
	if (m_IsPaused)
		return;

#if ENABLE_ASSERTS
	for (const CommandListState& state : m_CommandListData)
		gAssert(state.Queries.empty(), "The Queries inside the commandlist is not empty. This is because ExecuteCommandLists was not called with this commandlist.");
#endif
	m_CommandListMap.clear();

	for(QueryHeap& heap : m_QueryHeaps)
		heap.Resolve(m_FrameIndex);

	++m_FrameIndex;

	{
		for (QueryHeap& heap : m_QueryHeaps)
			heap.Reset(m_FrameIndex);

		ProfilerEventData& eventFrame = GetSampleFrame();
		eventFrame.NumEvents = 0;
		eventFrame.Allocator.Reset();
		for (uint32 i = 0; i < (uint32)m_Queues.size(); ++i)
			eventFrame.EventOffsetAndCountPerTrack[i] = {};

		QueryData& queryData = GetQueryData();
		queryData.Pairs.clear();
	}
}

void GPUProfiler::ExecuteCommandLists(ID3D12CommandQueue* pQueue, Span<ID3D12CommandList*> commandLists)
{
	if (!m_IsInitialized)
		return;

	if (m_IsPaused)
		return;

	auto it = m_QueueIndexMap.find(pQueue);
	if (it == m_QueueIndexMap.end())
		return;

	std::scoped_lock lock(m_QueryRangeLock);

	uint32 queueIndex				= it->second;
	ActiveEventStack& eventStack	= m_QueueEventStack[queueIndex];
	QueryData& queryData			= GetQueryData();
	ProfilerEventData& sampleFrame	= GetSampleFrame();

	// Ensure there are as many query pairs as there are events
	queryData.Pairs.resize(sampleFrame.Events.size());

	for (ID3D12CommandList* pCmd : commandLists)
	{
		CommandListState* pCommandListQueries = GetState(pCmd, false);
		if (pCommandListQueries)
		{
			for (CommandListState::Query& query : pCommandListQueries->Queries)
			{
				if (query.EventIndex != CommandListState::Query::EndEventFlag)
				{
					// If it's a "BeginEvent", add to the stack
					eventStack.Push() = query;
					if (query.EventIndex == CommandListState::Query::InvalidEventFlag)
						continue;

					ProfilerEvent& sampleEvent = sampleFrame.Events[query.EventIndex];
					sampleEvent.QueueIndex = queueIndex;
				}
				else
				{
					// If it's an "EndEvent", pop top query from the stack and pair up
					gAssert(eventStack.GetSize() > 0, "Event Begin/End mismatch");
					CommandListState::Query beginEventQuery = eventStack.Pop();
					if (beginEventQuery.EventIndex == CommandListState::Query::InvalidEventFlag)
						continue;

					// Pair up Begin/End query indices
					QueryData::QueryPair& pair	= queryData.Pairs[beginEventQuery.EventIndex];
					pair.QueryIndexBegin		= beginEventQuery.QueryIndex;
					pair.QueryIndexEnd			= query.QueryIndex;

					// Compute event depth
					ProfilerEvent& sampleEvent	= sampleFrame.Events[beginEventQuery.EventIndex];
					sampleEvent.Depth			= eventStack.GetSize();
					gAssert(sampleEvent.QueueIndex == queueIndex, "Begin/EndEvent must be recorded on the same queue");
				}
			}
			pCommandListQueries->Queries.clear();
		}
	}
}


GPUProfiler::CommandListState* GPUProfiler::GetState(ID3D12CommandList* pCmd, bool createIfNotFound)
{
	// See if it's already tracked
	AcquireSRWLockShared(&m_CommandListMapLock);
	auto it = m_CommandListMap.find(pCmd);
	uint32 index = it != m_CommandListMap.end() ? it->second : 0xFFFFFFFF;
	ReleaseSRWLockShared(&m_CommandListMapLock);

	if (index != 0xFFFFFFFF)
		return &m_CommandListData[index];

	if (createIfNotFound)
	{
		// If not, register new commandlist
		AcquireSRWLockExclusive(&m_CommandListMapLock);
		index = (uint32)m_CommandListMap.size();
		gAssert((uint32)index < m_CommandListData.size());
		m_CommandListMap[pCmd] = index;
		ReleaseSRWLockExclusive(&m_CommandListMapLock);

		return &m_CommandListData[index];
	}
	return nullptr;
}


void GPUProfiler::QueryHeap::Initialize(ID3D12Device* pDevice, ID3D12CommandQueue* pResolveQueue, uint32 maxNumQueries, uint32 frameLatency)
{
	m_pResolveQueue	= pResolveQueue;
	m_FrameLatency	= frameLatency;
	m_MaxNumQueries = maxNumQueries;

	D3D12_COMMAND_QUEUE_DESC queueDesc = pResolveQueue->GetDesc();

	D3D12_QUERY_HEAP_DESC heapDesc{};
	heapDesc.Count = maxNumQueries;
	heapDesc.NodeMask = 0x1;
	heapDesc.Type = queueDesc.Type == D3D12_COMMAND_LIST_TYPE_COPY ? D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP : D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
	pDevice->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&m_pQueryHeap));

	for (uint32 i = 0; i < frameLatency; ++i)
		pDevice->CreateCommandAllocator(queueDesc.Type, IID_PPV_ARGS(&m_CommandAllocators.emplace_back()));
	pDevice->CreateCommandList(0x1, queueDesc.Type, m_CommandAllocators[0], nullptr, IID_PPV_ARGS(&m_pCommandList));

	D3D12_RESOURCE_DESC readbackDesc = CD3DX12_RESOURCE_DESC::Buffer((uint64)maxNumQueries * sizeof(uint64) * frameLatency);
	D3D12_HEAP_PROPERTIES heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
	pDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &readbackDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_pReadbackResource));
	void* pReadbackData = nullptr;
	m_pReadbackResource->Map(0, nullptr, &pReadbackData);
	m_ReadbackData = Span<const uint64>(static_cast<uint64*>(pReadbackData), maxNumQueries * frameLatency);

	pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_pResolveFence));
	m_ResolveWaitHandle = CreateEventExA(nullptr, "Fence Event", 0, EVENT_ALL_ACCESS);
}

void GPUProfiler::QueryHeap::Shutdown()
{
	if (!IsInitialized())
		return;

	for (ID3D12CommandAllocator* pAllocator : m_CommandAllocators)
		pAllocator->Release();
	m_pCommandList->Release();
	m_pQueryHeap->Release();
	m_pReadbackResource->Release();
	m_pResolveFence->Release();
	CloseHandle(m_ResolveWaitHandle);
}

uint32 GPUProfiler::QueryHeap::RecordQuery(ID3D12GraphicsCommandList* pCmd)
{
	uint32 index = m_QueryIndex.fetch_add(1);
	if (index >= m_MaxNumQueries)
		return 0xFFFFFFFF;

	pCmd->EndQuery(m_pQueryHeap, D3D12_QUERY_TYPE_TIMESTAMP, index);
	return index;
}

uint32 GPUProfiler::QueryHeap::Resolve(uint32 frameIndex)
{
	if (!IsInitialized())
		return 0;

	uint32 frameBit = frameIndex % m_FrameLatency;
	uint32 queryStart = frameBit * m_MaxNumQueries;
	uint32 numQueries = Math::Min(m_MaxNumQueries, (uint32)m_QueryIndex);
	m_pCommandList->ResolveQueryData(m_pQueryHeap, D3D12_QUERY_TYPE_TIMESTAMP, 0, numQueries, m_pReadbackResource, queryStart * sizeof(uint64));
	m_pCommandList->Close();
	ID3D12CommandList* pCmdLists[] = { m_pCommandList };
	m_pResolveQueue->ExecuteCommandLists(1, pCmdLists);
	m_pResolveQueue->Signal(m_pResolveFence, frameIndex + 1);
	return numQueries;
}

void GPUProfiler::QueryHeap::Reset(uint32 frameIndex)
{
	if (!IsInitialized())
		return;

	m_QueryIndex = 0;
	ID3D12CommandAllocator* pAllocator = m_CommandAllocators[frameIndex % m_FrameLatency];
	pAllocator->Reset();
	m_pCommandList->Reset(pAllocator, nullptr);
}



//-----------------------------------------------------------------------------
// [SECTION] CPU Profiler
//-----------------------------------------------------------------------------


void CPUProfiler::Initialize(uint32 historySize)
{
	m_pEventData	= new ProfilerEventData[historySize];
	m_HistorySize	= historySize;
	m_IsInitialized = true;
}


void CPUProfiler::Shutdown()
{
	delete[] m_pEventData;
}


// Begin a new CPU event on the current thread
void CPUProfiler::BeginEvent(const char* pName, uint32 color, const char* pFilePath, uint32 lineNumber)
{
	if (!m_IsInitialized)
		return;

	if (m_EventCallback.OnEventBegin)
		m_EventCallback.OnEventBegin(pName, m_EventCallback.pUserData);

	if (m_Paused)
		return;

	// Record new event on TLS
	TLS& tls = GetTLS();
	tls.EventStack.Push()		= (uint32)tls.Events.size();

	ProfilerEvent& newEvent		= tls.Events.emplace_back();
	newEvent.Depth				= tls.EventStack.GetSize();
	newEvent.ThreadIndex		= tls.ThreadIndex;
	newEvent.pName				= GetData().Allocator.String(pName);
	newEvent.pFilePath			= pFilePath;
	newEvent.LineNumber			= lineNumber;
	newEvent.Color				= color == 0 ? ColorFromString(pName, 0.5f, 1.0f) : color;
	QueryPerformanceCounter((LARGE_INTEGER*)(&newEvent.TicksBegin));
}


// End the last pushed event on the current thread
void CPUProfiler::EndEvent()
{
	if (!m_IsInitialized)
		return;

	if (m_EventCallback.OnEventEnd)
		m_EventCallback.OnEventEnd(m_EventCallback.pUserData);

	if (m_Paused)
		return;

	// End and pop an event of the stack
	TLS& tls				= GetTLS();

	gAssert(tls.EventStack.GetSize() > 0, "Event mismatch. Called EndEvent more than BeginEvent");
	uint32 eventIndex		= tls.EventStack.Pop();
	ProfilerEvent& event	= tls.Events[eventIndex];
	QueryPerformanceCounter((LARGE_INTEGER*)(&event.TicksEnd));
}


// Process the current frame and advance to the next
void CPUProfiler::Tick()
{
	if (!m_IsInitialized)
		return;

	m_Paused = m_QueuedPaused;
	if (m_Paused || !m_pEventData)
		return;

	// End the "CPU Frame" event (except on frame 0)
	if (m_FrameIndex)
		EndEvent();

	std::scoped_lock lock(m_ThreadDataLock);

	// Collect recorded events from all threads
	ProfilerEventData& data = GetData();
	data.NumEvents = (uint32)data.Events.size();
	data.EventOffsetAndCountPerTrack.resize(m_ThreadData.size());
	data.Events.clear();
	for (uint32 threadIndex = 0; threadIndex < (uint32)m_ThreadData.size(); ++threadIndex)
	{
		ThreadData& threadData = m_ThreadData[threadIndex];

		// Check if all threads have ended all open sample events
		gAssert(threadData.pTLS->EventStack.GetSize() == 0, "Thread %s has not closed all events", threadData.Name);

		// Copy all events for the thread to the common array
		// Keep track of which range of events belong to what thread
		data.EventOffsetAndCountPerTrack[threadIndex] = ProfilerEventData::OffsetAndSize((uint32)data.Events.size(), (uint32)threadData.pTLS->Events.size());
		data.Events.insert(data.Events.end(), threadData.pTLS->Events.begin(), threadData.pTLS->Events.end());
		threadData.pTLS->Events.clear();
	}

	// Advance the frame and reset its data
	++m_FrameIndex;

	ProfilerEventData& newData = GetData();
	newData.Allocator.Reset();
	newData.NumEvents = 0;

	// Begin a "CPU Frame" event
	BeginEvent("CPU Frame");
}


// Register a new thread
void CPUProfiler::RegisterThread(const char* pName)
{
	TLS& tls = GetTLSUnsafe();
	gAssert(!tls.IsInitialized);
	tls.IsInitialized	= true;
	std::scoped_lock lock(m_ThreadDataLock);
	tls.ThreadIndex		= (uint32)m_ThreadData.size();
	ThreadData& data	= m_ThreadData.emplace_back();

	// If the name is not provided, retrieve it using GetThreadDescription()
	if (pName)
	{
		strcpy_s(data.Name, ARRAYSIZE(data.Name), pName);
	}
	else
	{
		PWSTR pDescription = nullptr;
		VERIFY_HR(::GetThreadDescription(GetCurrentThread(), &pDescription));
		size_t converted = 0;
		gVerify(wcstombs_s(&converted, data.Name, ARRAYSIZE(data.Name), pDescription, ARRAYSIZE(data.Name)), == 0);
	}
	data.ThreadID	= GetCurrentThreadId();
	data.pTLS		= &tls;
	data.Index		= (uint32)m_ThreadData.size() - 1;
}

#endif
