#include "Common.hlsli"
#include "HZB.hlsli"
#include "D3D12.hlsli"
#include "VisibilityBuffer.hlsli"
#include "WaveOps.hlsli"
#include "ShaderDebugRender.hlsli"

/*
	-- 2 Phase Occlusion Culling --

	Works under the assumption that it's likely that objects visible in the previous frame, will be visible this frame.

	In Phase 1, we render all objects that were visible last frame by testing against the previous HZB.
	Occluded objects are stored in a list, to be processed later.
	The HZB is constructed from the current result.
	Phase 2 tests all previously occluded objects against the new HZB and renders unoccluded.
	The HZB is constructed again from this result to be used in the next frame.

	Cull both on a per-instance level as on a per-meshlet level.
	Leverage Mesh/Amplification shaders to drive per-meshlet culling.

	https://advances.realtimerendering.com/s2015/aaltonenhaar_siggraph2015_combined_final_footer_220dpi.pdf
*/

// Debug draw bounding box around occluded instances
#define VISUALIZE_OCCLUDED 0

#ifndef OCCLUSION_FIRST_PASS
#define OCCLUSION_FIRST_PASS 1
#endif

#ifndef ENABLE_DEBUG_DATA
#define ENABLE_DEBUG_DATA 0
#endif

#ifndef OCCLUSION_CULL
#define OCCLUSION_CULL 1
#endif

#define NUM_CULL_INSTANCES_THREADS 64

// Element index of counter for total amount of candidate meshlets.
static const int COUNTER_TOTAL_CANDIDATE_MESHLETS 	= 0;
// Element index of counter for amount of candidate meshlets in Phase 1.
static const int COUNTER_PHASE1_CANDIDATE_MESHLETS 	= 1;
// Element index of counter for amount of candidate meshlets in Phase 2.
static const int COUNTER_PHASE2_CANDIDATE_MESHLETS 	= 2;
// Element index of counter for amount of visible meshlets in Phase 1.
static const int COUNTER_PHASE1_VISIBLE_MESHLETS 	= 0;
// Element index of counter for amount of visible meshlets in Phase 2.
static const int COUNTER_PHASE2_VISIBLE_MESHLETS 	= 1;

#if OCCLUSION_FIRST_PASS
static const int MeshletCounterIndex = COUNTER_PHASE1_CANDIDATE_MESHLETS;
static const int VisibleMeshletCounter = COUNTER_PHASE1_VISIBLE_MESHLETS;
#else
static const int MeshletCounterIndex = COUNTER_PHASE2_CANDIDATE_MESHLETS;
static const int VisibleMeshletCounter = COUNTER_PHASE2_VISIBLE_MESHLETS;
#endif

RWStructuredBuffer<MeshletCandidate> uCandidateMeshlets : 			register(u0);
RWBuffer<uint> uCounter_CandidateMeshlets : 						register(u1);
RWStructuredBuffer<uint> uPhaseTwoInstances : 						register(u2);
RWBuffer<uint> uCounter_PhaseTwoInstances : 						register(u3);
RWStructuredBuffer<MeshletCandidate> uVisibleMeshlets :				register(u4);
RWBuffer<uint> uCounter_VisibleMeshlets : 							register(u5);

RWStructuredBuffer<D3D12_DISPATCH_ARGUMENTS> uDispatchArguments : 	register(u0);

StructuredBuffer<uint> tPhaseTwoInstances : 						register(t0);
Buffer<uint> tCounter_CandidateMeshlets : 							register(t1);
Buffer<uint> tCounter_PhaseTwoInstances : 							register(t2);
Buffer<uint> tCounter_VisibleMeshlets : 							register(t3);
StructuredBuffer<MeshletCandidate> tVisibleMeshlets : 				register(t4);
Texture2D<float> tHZB : 											register(t3);

StructuredBuffer<uint> tBinnedMeshlets : 							register(t2);
StructuredBuffer<uint4> tMeshletBinData :							register(t3);

uint GetCandidateMeshletOffset(bool phase2)
{
	return phase2 ? uCounter_CandidateMeshlets[COUNTER_PHASE1_CANDIDATE_MESHLETS] : 0u;
}

uint GetNumInstances()
{
#if OCCLUSION_FIRST_PASS
    return cView.NumInstances;
#else
    return tCounter_PhaseTwoInstances[0];
#endif
}

InstanceData GetInstanceForThread(uint threadID)
{
#if OCCLUSION_FIRST_PASS
    return GetInstance(threadID);
#else
	return GetInstance(tPhaseTwoInstances[threadID]);
#endif
}

[numthreads(NUM_CULL_INSTANCES_THREADS, 1, 1)]
void CullInstancesCS(uint threadID : SV_DispatchThreadID)
{
	uint numInstances = GetNumInstances();
    if(threadID >= numInstances)
        return;

    InstanceData instance = GetInstanceForThread(threadID);
    MeshData mesh = GetMesh(instance.MeshIndex);

	FrustumCullData cullData = FrustumCull(instance.LocalBoundsOrigin, instance.LocalBoundsExtents, instance.LocalToWorld, cView.ViewProjection);
	bool isVisible = cullData.IsVisible;
	bool wasOccluded = false;

#if OCCLUSION_CULL
	if(isVisible)
	{
#if OCCLUSION_FIRST_PASS
		FrustumCullData prevCullData = FrustumCull(instance.LocalBoundsOrigin, instance.LocalBoundsExtents, instance.LocalToWorldPrev, cView.ViewProjectionPrev);
		wasOccluded = !HZBCull(prevCullData, tHZB);

		// If the instance was occluded the previous frame, we can't be sure it's still occluded this frame.
		// Add it to the list to re-test in the second phase.
		if(wasOccluded)
		{
			uint elementOffset = 0;
			InterlockedAdd_WaveOps(uCounter_PhaseTwoInstances, 0, 1, elementOffset);
			uPhaseTwoInstances[elementOffset] = instance.ID;
		}
#else
		isVisible = HZBCull(cullData, tHZB);
#endif
	}
#endif

	// If instance is visible and wasn't occluded in the previous frame, submit it
    if(isVisible && !wasOccluded)
    {
		// Limit meshlet count to how large our buffer is
		uint globalMeshletIndex;
        InterlockedAdd_Varying_WaveOps(uCounter_CandidateMeshlets, COUNTER_TOTAL_CANDIDATE_MESHLETS, mesh.MeshletCount, globalMeshletIndex);
		uint clampedNumMeshlets = min(globalMeshletIndex + mesh.MeshletCount, MAX_NUM_MESHLETS);
		uint numMeshletsToAdd = max(clampedNumMeshlets - globalMeshletIndex, 0);

		uint elementOffset;
		InterlockedAdd_Varying_WaveOps(uCounter_CandidateMeshlets, MeshletCounterIndex, numMeshletsToAdd, elementOffset);

		uint meshletCandidateOffset = GetCandidateMeshletOffset(!OCCLUSION_FIRST_PASS);
		for(uint i = 0; i < numMeshletsToAdd; ++i)
		{
			MeshletCandidate meshlet;
			meshlet.InstanceID = instance.ID;
			meshlet.MeshletIndex = i;
			uCandidateMeshlets[meshletCandidateOffset + elementOffset + i] = meshlet;
		}
    }

#if VISUALIZE_OCCLUDED
	if(wasOccluded)
	{
		DrawOBB(instance.LocalBoundsOrigin, instance.LocalBoundsExtents, instance.LocalToWorld, Colors::Green);
	}
#endif
}

[numthreads(1, 1, 1)]
void BuildMeshletCullIndirectArgs()
{
    uint numMeshlets = tCounter_CandidateMeshlets[MeshletCounterIndex];
    D3D12_DISPATCH_ARGUMENTS args;
    args.ThreadGroupCount = uint3(DivideAndRoundUp(numMeshlets, NUM_CULL_INSTANCES_THREADS), 1, 1);
    uDispatchArguments[0] = args;
}

[numthreads(1, 1, 1)]
void BuildInstanceCullIndirectArgs()
{
    uint numInstances = tCounter_PhaseTwoInstances[0];
    D3D12_DISPATCH_ARGUMENTS args;
    args.ThreadGroupCount = uint3(DivideAndRoundUp(numInstances, NUM_CULL_INSTANCES_THREADS), 1, 1);
    uDispatchArguments[0] = args;
}

[numthreads(NUM_CULL_INSTANCES_THREADS, 1, 1)]
void CullMeshletsCS(uint threadID : SV_DispatchThreadID)
{
	if(threadID < uCounter_CandidateMeshlets[MeshletCounterIndex])
	{
		uint candidateIndex = GetCandidateMeshletOffset(!OCCLUSION_FIRST_PASS) + threadID;
		MeshletCandidate candidate = uCandidateMeshlets[candidateIndex];
		InstanceData instance = GetInstance(candidate.InstanceID);
		MeshData mesh = GetMesh(instance.MeshIndex);
		Meshlet::Bounds bounds = BufferLoad<Meshlet::Bounds>(mesh.BufferIndex, candidate.MeshletIndex, mesh.MeshletBoundsOffset);
		FrustumCullData cullData = FrustumCull(bounds.Center, bounds.Extents, instance.LocalToWorld, cView.ViewProjection);
		bool isVisible = cullData.IsVisible;
		bool wasOccluded = false;

#if OCCLUSION_CULL
		if(isVisible)
		{
#if OCCLUSION_FIRST_PASS
			FrustumCullData prevCullData = FrustumCull(bounds.Center, bounds.Extents, instance.LocalToWorldPrev, cView.ViewProjectionPrev);
			if(prevCullData.IsVisible)
			{
				wasOccluded = !HZBCull(prevCullData, tHZB);
			}

			// If the meshlet was occluded the previous frame, we can't be sure it's still occluded this frame.
			// Add it to the list to re-test in the second phase.
			if(wasOccluded)
			{
				// Limit how many meshlets we're writing based on the buffer size
				uint globalMeshletIndex;
        		InterlockedAdd_WaveOps(uCounter_CandidateMeshlets, COUNTER_TOTAL_CANDIDATE_MESHLETS, 1, globalMeshletIndex);
				if(globalMeshletIndex < MAX_NUM_MESHLETS)
				{
					uint elementOffset;
					InterlockedAdd_WaveOps(uCounter_CandidateMeshlets, COUNTER_PHASE2_CANDIDATE_MESHLETS, 1, elementOffset);
					uCandidateMeshlets[GetCandidateMeshletOffset(true) + elementOffset] = candidate;
				}
			}
#else
			isVisible = HZBCull(cullData, tHZB);
#endif
		}
#endif

		// If meshlet is visible and wasn't occluded in the previous frame, submit it
		if(isVisible && !wasOccluded)
		{
			uint elementOffset;
			InterlockedAdd_WaveOps(uCounter_VisibleMeshlets, VisibleMeshletCounter, 1, elementOffset);

#if !OCCLUSION_FIRST_PASS
			elementOffset += uCounter_VisibleMeshlets[COUNTER_PHASE1_VISIBLE_MESHLETS];
#endif
			uVisibleMeshlets[elementOffset] = candidate;
		}
	}
}

[numthreads(1, 1, 1)]
void PrintStatsCS()
{
	uint numInstances = cView.NumInstances;
	uint numMeshlets = 0;
	for(uint i = 0; i < numInstances; ++i)
	{
		InstanceData instance = GetInstance(i);
		MeshData mesh = GetMesh(instance.MeshIndex);
		numMeshlets += mesh.MeshletCount;
	}

	uint occludedInstances = tCounter_PhaseTwoInstances[0];
	uint visibleInstances = numInstances - occludedInstances;
	uint processedMeshlets = tCounter_CandidateMeshlets[COUNTER_TOTAL_CANDIDATE_MESHLETS];
	uint phase1CandidateMeshlets = tCounter_CandidateMeshlets[COUNTER_PHASE1_CANDIDATE_MESHLETS];
	uint phase2CandidateMeshlets = tCounter_CandidateMeshlets[COUNTER_PHASE2_CANDIDATE_MESHLETS];
	uint phase1VisibleMeshlets = tCounter_VisibleMeshlets[COUNTER_PHASE1_VISIBLE_MESHLETS];
	uint phase2VisibleMeshlets = tCounter_VisibleMeshlets[COUNTER_PHASE2_VISIBLE_MESHLETS];

	TextWriter writer = CreateTextWriter(float2(20, 20));

	writer = writer + '-' + '-' + '-' + 'S' + 'c' + 'e' + 'n' + 'e' + '-' + '-' + '-';
	writer.NewLine();
	writer = writer + 'T' + 'o' + 't' + 'a'  + 'l'  + ' ';
	writer = writer + 'i' + 'n' + 's' + 't'  + 'a'  + 'n'  + 'c'  + 'e'  + 's' + ' ';
	writer.Int(numInstances);
	writer.NewLine();

	writer = writer + 'T' + 'o' + 't' + 'a'  + 'l'  + ' ';
	writer = writer + 'm' + 'e' + 's' + 'h'  + 'l'  + 'e'  + 't'  + 's'  + ' ';
	writer.Int(numMeshlets);
	writer.NewLine();

	writer = writer + 'T' + 'o' + 't' + 'a'  + 'l' + ' ';
	writer = writer + 'p' + 'r' + 'o' + 'c'  + 'e'  + 's'  + 's'  + 'e' + 'd' + ' ';
	writer = writer + 'm' + 'e' + 's' + 'h'  + 'l'  + 'e'  + 't'  + 's'  + ' ';
	writer.Int(processedMeshlets);
	writer.NewLine();

	writer = writer + '-' + '-' + '-' + 'P' + 'h' + 'a' + 's' + 'e' + ' ' + '1' + '-' + '-' + '-';
	writer.NewLine();

	writer = writer + 'P' + 'r' + 'o' + 'c'  + 'e'  + 's'  + 's'  + 'e' + 'd' + ' ';
	writer = writer + 'm' + 'e' + 's' + 'h'  + 'l'  + 'e'  + 't'  + 's' + ' ';
	writer.Int(phase1CandidateMeshlets);
	writer.NewLine();

	writer = writer + 'V' + 'i' + 's' + 'i' + 'b' + 'l' + 'e' + ' ';
	writer = writer + 'm' + 'e' + 's' + 'h'  + 'l'  + 'e'  + 't'  + 's'  + ' ';
	writer.Int(phase1VisibleMeshlets);
	writer.NewLine();

	writer = writer + '-' + '-' + '-' + 'P' + 'h' + 'a' + 's' + 'e' + ' ' + '2' + '-' + '-' + '-';
	writer.NewLine();

	writer = writer + 'P' + 'r' + 'o' + 'c'  + 'e'  + 's'  + 's'  + 'e' + 'd' + ' ';
	writer = writer + 'i' + 'n' + 's' + 't'  + 'a'  + 'n'  + 'c'  + 'e'  + 's' + ' ';
	writer.Int(occludedInstances);
	writer.NewLine();

	writer = writer + 'P' + 'r' + 'o' + 'c'  + 'e'  + 's'  + 's'  + 'e' + 'd' + ' ';
	writer = writer + 'm' + 'e' + 's' + 'h'  + 'l'  + 'e'  + 't'  + 's' + ' ';
	writer.Int(phase2CandidateMeshlets);
	writer.NewLine();

	writer = writer + 'V' + 'i' + 's' + 'i' + 'b' + 'l' + 'e' + ' ';
	writer = writer + 'm' + 'e' + 's' + 'h'  + 'l'  + 'e'  + 't'  + 's'  + ' ';
	writer.Int(phase2VisibleMeshlets);
	writer.NewLine();
}
