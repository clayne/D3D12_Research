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

// If enabled, perform 2 phase occlusion culling
// If disabled, no occlusion culling and only 1 pass.
#ifndef OCCLUSION_CULL
#define OCCLUSION_CULL 1
#endif

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
static const int MeshletCounterIndex = COUNTER_PHASE1_CANDIDATE_MESHLETS;	// Index of counter for candidate meshlets in current phase.
static const int VisibleMeshletCounter = COUNTER_PHASE1_VISIBLE_MESHLETS;	// Index of counter for visible meshlets in current phase.
#else
static const int MeshletCounterIndex = COUNTER_PHASE2_CANDIDATE_MESHLETS;	// Index of counter for candidate meshlets in current phase.
static const int VisibleMeshletCounter = COUNTER_PHASE2_VISIBLE_MESHLETS;	// Index of counter for visible meshlets in current phase.
#endif

// Returns the offset in the candidate meshlet buffer for the current phase
uint GetCandidateMeshletOffset(bool phase2, RWStructuredBufferH<uint> candidateMeshletsCounter)
{
	return phase2 ? candidateMeshletsCounter[COUNTER_PHASE1_CANDIDATE_MESHLETS] : 0u;
}


struct ClearUAVParams
{
	RWStructuredBufferH<uint> Counter_CandidateMeshlets;	// Number of meshlets to process
	RWStructuredBufferH<uint> Counter_PhaseTwoInstances;	// Number of instances which need to be tested in Phase 2
	RWStructuredBufferH<uint> Counter_VisibleMeshlets;		// Number of meshlets to rasterize
};
DEFINE_CONSTANTS(ClearUAVParams, 0);

[numthreads(1, 1, 1)]
void ClearCountersCS()
{
	ClearUAVParams params = cClearUAVParams;
	params.Counter_CandidateMeshlets.Store(0, 0);
	params.Counter_CandidateMeshlets.Store(1, 0);
	params.Counter_CandidateMeshlets.Store(2, 0);

	params.Counter_PhaseTwoInstances.Store(0, 0);

	params.Counter_VisibleMeshlets.Store(0, 0);
	params.Counter_VisibleMeshlets.Store(1, 0);
}


struct InstanceCullArgsParams
{
	StructuredBufferH<uint> Counter_PhaseTwoInstances;					// Number of instances which need to be tested in Phase 2
	RWStructuredBufferH<D3D12_DISPATCH_ARGUMENTS> DispatchArguments; 	// General purpose dispatch args
};
DEFINE_CONSTANTS(InstanceCullArgsParams, 0);

[numthreads(1, 1, 1)]
void BuildInstanceCullIndirectArgs()
{
	InstanceCullArgsParams params = cInstanceCullArgsParams;
    uint numInstances = min(params.Counter_PhaseTwoInstances[0], MAX_NUM_INSTANCES);
    D3D12_DISPATCH_ARGUMENTS args;
    args.ThreadGroupCount = uint3(DivideAndRoundUp(numInstances, NUM_CULL_INSTANCES_THREADS), 1, 1);
    params.DispatchArguments.Store(0, args);
}


/*
	Per-instance culling
*/
struct InstanceCullParams
{
	uint2 HZBDimensions;
	RWStructuredBufferH<MeshletCandidate> CandidateMeshlets;	// List of meshlets to process
	RWStructuredBufferH<uint> Counter_CandidateMeshlets;		// Number of meshlets to process
	RWStructuredBufferH<uint> PhaseTwoInstances;				// List of instances which need to be tested in Phase 2
	RWStructuredBufferH<uint> Counter_PhaseTwoInstances;		// Number of instances which need to be tested in Phase 2
	Texture2DH<float> HZB;										// Current HZB texture
};
DEFINE_CONSTANTS(InstanceCullParams, 0);

[numthreads(NUM_CULL_INSTANCES_THREADS, 1, 1)]
void CullInstancesCS(uint threadID : SV_DispatchThreadID)
{
#if OCCLUSION_FIRST_PASS
	uint numInstances = cView.NumInstances;
#else
	uint numInstances = cInstanceCullParams.Counter_PhaseTwoInstances[0];
#endif

	if(threadID >= numInstances)
        return;

	uint instanceIndex = threadID;
#if !OCCLUSION_FIRST_PASS
	instanceIndex = cInstanceCullParams.PhaseTwoInstances[instanceIndex];
#endif

	InstanceData instance = GetInstance(instanceIndex);

#if OCCLUSION_FIRST_PASS
	MaterialData material = GetMaterial(instance.MaterialIndex);
	if(material.RasterBin == 0xFFFFFFFF)
		return;
#endif

    MeshData mesh = GetMesh(instance.MeshIndex);

	// Frustum test instance against the current view
	FrustumCullData cullData = FrustumCull(instance.LocalBoundsOrigin, instance.LocalBoundsExtents, instance.LocalToWorld, cView.WorldToClip);
	bool isVisible = cullData.IsVisible;
	bool wasOccluded = false;

#if OCCLUSION_CULL
	if(isVisible)
	{
#if OCCLUSION_FIRST_PASS
		// Frustum test instance against the *previous* view to determine if it was visible last frame
		FrustumCullData prevCullData = FrustumCull(instance.LocalBoundsOrigin, instance.LocalBoundsExtents, instance.LocalToWorldPrev, cView.WorldToClipPrev);
		if (prevCullData.IsVisible)
		{
			// Occlusion test instance against the HZB
			wasOccluded = !HZBCull(prevCullData, cInstanceCullParams.HZB.Get(), cInstanceCullParams.HZBDimensions);
		}

		// If the instance was occluded the previous frame, we can't be sure it's still occluded this frame.
		// Add it to the list to re-test in the second phase.
		if(wasOccluded)
		{
			uint elementOffset = 0;
			InterlockedAdd_WaveOps(cInstanceCullParams.Counter_PhaseTwoInstances.Get(), 0, 1, elementOffset);
			if(elementOffset < MAX_NUM_INSTANCES)
				cInstanceCullParams.PhaseTwoInstances.Store(elementOffset, instance.ID);
		}
#else
		// Occlusion test instance against the updated HZB
		isVisible = HZBCull(cullData, cInstanceCullParams.HZB.Get(), cInstanceCullParams.HZBDimensions);
#endif
	}
#endif

	// If instance is visible and wasn't occluded in the previous frame, submit it
    if(isVisible && !wasOccluded)
    {
		// Limit meshlet count to how large our buffer is
		uint globalMeshletIndex;
        InterlockedAdd_Varying_WaveOps(cInstanceCullParams.Counter_CandidateMeshlets.Get(), COUNTER_TOTAL_CANDIDATE_MESHLETS, mesh.MeshletCount, globalMeshletIndex);
		int clampedNumMeshlets = min(globalMeshletIndex + mesh.MeshletCount, MAX_NUM_MESHLETS);
		int numMeshletsToAdd = max(clampedNumMeshlets - (int)globalMeshletIndex, 0);

		// Add all meshlets of current instance to the candidate meshlets
		uint elementOffset;
		InterlockedAdd_Varying_WaveOps(cInstanceCullParams.Counter_CandidateMeshlets.Get(), MeshletCounterIndex, numMeshletsToAdd, elementOffset);
		uint meshletCandidateOffset = GetCandidateMeshletOffset(!OCCLUSION_FIRST_PASS, cInstanceCullParams.Counter_CandidateMeshlets);
		for(uint i = 0; i < numMeshletsToAdd; ++i)
		{
			MeshletCandidate meshlet;
			meshlet.InstanceID = instance.ID;
			meshlet.MeshletIndex = i;
			cInstanceCullParams.CandidateMeshlets.Store(meshletCandidateOffset + elementOffset + i, meshlet);
		}
    }

#if VISUALIZE_OCCLUDED
	if(wasOccluded)
	{
		DrawOBB(instance.LocalBoundsOrigin, instance.LocalBoundsExtents, instance.LocalToWorld, Colors::Green);
	}
#endif
}


struct MeshletCullArgsParams
{
	RWStructuredBufferH<D3D12_DISPATCH_ARGUMENTS> DispatchArguments; 	// General purpose dispatch args
	StructuredBufferH<uint> Counter_CandidateMeshlets;					// Number of meshlets to process	
};
DEFINE_CONSTANTS(MeshletCullArgsParams, 0);


[numthreads(1, 1, 1)]
void BuildMeshletCullIndirectArgs()
{
    uint numMeshlets = cMeshletCullArgsParams.Counter_CandidateMeshlets[MeshletCounterIndex];
    D3D12_DISPATCH_ARGUMENTS args;
    args.ThreadGroupCount = uint3(DivideAndRoundUp(numMeshlets, NUM_CULL_MESHLETS_THREADS), 1, 1);
    cMeshletCullArgsParams.DispatchArguments.Store(0, args);
}


/*
	Per-meshlet culling
*/
struct MeshletCullParams
{
	uint2 HZBDimensions;
	RWStructuredBufferH<MeshletCandidate> CandidateMeshlets;	// List of meshlets to process
	RWStructuredBufferH<uint> Counter_CandidateMeshlets;		// Number of meshlets to process
	RWStructuredBufferH<MeshletCandidate> VisibleMeshlets;		// List of meshlets to rasterize
	RWStructuredBufferH<uint> Counter_VisibleMeshlets;			// Number of meshlets to rasterize	
	Texture2DH<float> HZB;										// Current HZB texture
};
DEFINE_CONSTANTS(MeshletCullParams, 0);

[numthreads(NUM_CULL_MESHLETS_THREADS, 1, 1)]
void CullMeshletsCS(uint threadID : SV_DispatchThreadID)
{
	if(threadID < cMeshletCullParams.Counter_CandidateMeshlets[MeshletCounterIndex])
	{
		uint candidateIndex = GetCandidateMeshletOffset(!OCCLUSION_FIRST_PASS, cMeshletCullParams.Counter_CandidateMeshlets) + threadID;
		MeshletCandidate candidate = cMeshletCullParams.CandidateMeshlets[candidateIndex];
		InstanceData instance = GetInstance(candidate.InstanceID);
		MeshData mesh = GetMesh(instance.MeshIndex);

		// Frustum test meshlet against the current view
		Meshlet::Bounds bounds = mesh.DataBuffer.LoadStructure<Meshlet::Bounds>(candidate.MeshletIndex, mesh.MeshletBoundsOffset);
		FrustumCullData cullData = FrustumCull(bounds.LocalCenter, bounds.LocalExtents, instance.LocalToWorld, cView.WorldToClip);
		bool isVisible = cullData.IsVisible;
		bool wasOccluded = false;

#if OCCLUSION_CULL
		if(isVisible)
		{
#if OCCLUSION_FIRST_PASS
			// Frustum test meshlet against the *previous* view to determine if it was visible last frame
			FrustumCullData prevCullData = FrustumCull(bounds.LocalCenter, bounds.LocalExtents, instance.LocalToWorldPrev, cView.WorldToClipPrev);
			if(prevCullData.IsVisible)
			{
				// Occlusion test meshlet against the HZB
				wasOccluded = !HZBCull(prevCullData, cMeshletCullParams.HZB.Get(), cMeshletCullParams.HZBDimensions);
			}

			// If the meshlet was occluded the previous frame, we can't be sure it's still occluded this frame.
			// Add it to the list to re-test in the second phase.
			if(wasOccluded)
			{
				// Limit how many meshlets we're writing based on the buffer size
				uint globalMeshletIndex;
        		InterlockedAdd_WaveOps(cMeshletCullParams.Counter_CandidateMeshlets.Get(), COUNTER_TOTAL_CANDIDATE_MESHLETS, 1, globalMeshletIndex);
				if(globalMeshletIndex < MAX_NUM_MESHLETS)
				{
					uint elementOffset;
					InterlockedAdd_WaveOps(cMeshletCullParams.Counter_CandidateMeshlets.Get(), COUNTER_PHASE2_CANDIDATE_MESHLETS, 1, elementOffset);
					cMeshletCullParams.CandidateMeshlets.Store(GetCandidateMeshletOffset(true, cMeshletCullParams.Counter_CandidateMeshlets) + elementOffset, candidate);
				}
			}
#else
			// Occlusion test meshlet against the updated HZB
			isVisible = HZBCull(cullData, cMeshletCullParams.HZB.Get(), cMeshletCullParams.HZBDimensions);
#endif
		}
#endif

		// If meshlet is visible and wasn't occluded in the previous frame, submit it
		if(isVisible && !wasOccluded)
		{
			uint elementOffset;
			InterlockedAdd_WaveOps(cMeshletCullParams.Counter_VisibleMeshlets.Get(), VisibleMeshletCounter, 1, elementOffset);

#if !OCCLUSION_FIRST_PASS
			elementOffset += cMeshletCullParams.Counter_VisibleMeshlets[COUNTER_PHASE1_VISIBLE_MESHLETS];
#endif
			cMeshletCullParams.VisibleMeshlets.Store(elementOffset, candidate);
		}
	}
}

struct StatsParams
{
	float2 Pos;
	uint NumBins;
	StructuredBufferH<uint> Counter_CandidateMeshlets;	// Number of meshlets to process
	StructuredBufferH<uint> Counter_PhaseTwoInstances;	// Number of instances which need to be tested in Phase 2
	StructuredBufferH<uint> Counter_VisibleMeshlets;	// Number of meshlets to rasterize
	StructuredBufferH<uint4> BinnedMeshletOffsetAndCounts[2];
};
DEFINE_CONSTANTS(StatsParams, 0);

/*
	Debug statistics
*/
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

	uint occludedInstances = cStatsParams.Counter_PhaseTwoInstances[0];
	uint visibleInstances = numInstances - occludedInstances;
	uint processedMeshlets = cStatsParams.Counter_CandidateMeshlets[COUNTER_TOTAL_CANDIDATE_MESHLETS];
	uint phase1CandidateMeshlets = cStatsParams.Counter_CandidateMeshlets[COUNTER_PHASE1_CANDIDATE_MESHLETS];
	uint phase2CandidateMeshlets = cStatsParams.Counter_CandidateMeshlets[COUNTER_PHASE2_CANDIDATE_MESHLETS];
	uint phase1VisibleMeshlets = cStatsParams.Counter_VisibleMeshlets[COUNTER_PHASE1_VISIBLE_MESHLETS];
	uint phase2VisibleMeshlets = cStatsParams.Counter_VisibleMeshlets[COUNTER_PHASE2_VISIBLE_MESHLETS];

	TextWriter writer = CreateTextWriter(cStatsParams.Pos);

	uint align = 280;

	/*
		Totals
	*/
	writer.Text(TEXT("--- Scene ---"));
	writer.NewLine();

	writer.Text(TEXT("Total meshlets: "));
	writer.LeftAlign(align);
	writer.Int(numMeshlets, true);

	writer.NewLine();

	writer.Text(TEXT("Total processed meshlets: "));

	int numProcessedMeshletsCapped = min(MAX_NUM_MESHLETS, processedMeshlets);
	writer.LeftAlign(align);
	writer.Int(numProcessedMeshletsCapped, true);
	if(numProcessedMeshletsCapped < processedMeshlets)
	{
		writer.SetColor(float4(1, 0, 0, 1));
		writer.Text(TEXT(" (+"));
		writer.LeftAlign(align);
		writer.Int(processedMeshlets - numProcessedMeshletsCapped, true);
		writer.Text(TEXT(")"));
		writer.SetColor(float4(1, 1, 1, 1));
	}

	writer.NewLine();
	writer.NewLine();

	/*
		Phase 1
	*/
	writer.Text(TEXT("--- Phase 1 ---"));
	writer.NewLine();

	writer.Text(TEXT("Input instances: "));
	writer.LeftAlign(align);
	writer.Int(numInstances, true);
	writer.NewLine();

	writer.Text(TEXT("Input meshlets: "));
	writer.LeftAlign(align);
	writer.Int(phase1CandidateMeshlets, true);
	writer.NewLine();

	writer.Text(TEXT("Occluded instances: "));
	writer.LeftAlign(align);
	writer.Int(occludedInstances, true);
	writer.NewLine();

	writer.Text(TEXT("Occluded meshlets: "));
	writer.LeftAlign(align);
	writer.Int(phase2CandidateMeshlets, true);
	writer.NewLine();

	writer.Text(TEXT("Visible meshlets: "));
	writer.LeftAlign(align);
	writer.SetColor(Colors::Green);
	writer.Int(phase1VisibleMeshlets, true);
	writer.SetColor(Colors::White);
	writer.NewLine();
	writer.NewLine();

	writer.Text(TEXT("--- Phase 2 ---"));
	writer.NewLine();

	writer.Text(TEXT("Occluded instances: "));
	writer.LeftAlign(align);
	writer.Int(occludedInstances, true);
	writer.NewLine();

	writer.Text(TEXT("Input meshlets: "));
	writer.LeftAlign(align);
	writer.Int(phase2CandidateMeshlets, true);
	writer.NewLine();

	writer.Text(TEXT("Visible meshlets: "));
	writer.LeftAlign(align);
	writer.SetColor(Colors::Green);
	writer.Int(phase2VisibleMeshlets, true);
	writer.SetColor(Colors::White);
	writer.NewLine();
	writer.NewLine();

	writer.Text(TEXT("--- Bins ---"));
	writer.NewLine();

	for(int p = 0; p < 2; ++p)
	{
		writer.Text(TEXT("Phase "));
		writer.Int(p + 1);
		writer.NewLine();
		for(int i = 0; i < cStatsParams.NumBins; ++i)
		{
			writer.Text(TEXT("Bin "));
			writer.Int(i, false);
			writer.Text(':');
			writer.Text(' ');

			uint4 offsetAndCount  = cStatsParams.BinnedMeshletOffsetAndCounts[p][i];
			writer.Text(TEXT("Count: "));
			writer.Int(offsetAndCount.x);

			writer.LeftAlign(200);
			writer.Text(TEXT(" Offset: "));
			writer.Int(offsetAndCount.w);

			writer.NewLine();
		}
	}
}
