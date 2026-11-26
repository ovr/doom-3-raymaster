// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

/*
	The acceleration structures of the GPU scene (R15), built on the render graph after each applied update:

	  rt skin          COMPUTE    the skinned instances' vertices (internal/rtSkin.hlsl), idDrawVert's layout
	  rt blas          AS_BUILD   new static BLAS and every skinned one
	  rt blas compact  AS_BUILD   the BLAS whose compacted sizes came back, copied at that size
	  rt tlas          AS_BUILD   every instance whose BLAS exists: InstanceID its first geometry record, the front
	                              face the side Doom 3 draws (TRIANGLE_FRONT_COUNTERCLOCKWISE)
	  rt blas sizes    AS_BUILD   the compacted sizes of static BLAS not asked yet, while none are in flight
	  rt blas readback COPY       those sizes into a READBACK buffer

	A static BLAS is one per blasKey (a crate drawn 50 times is one), checked against its geometries; it is kept
	while instances use it and unusedBuilds builds after, and dropped with its static generation. After a full
	update a build makes every one the scene lacks, so that frame traces all its casters; after a delta new ones up
	to scratchBudget of scratch, and the instances of the rest wait. What a full update could not make (a buffer, a
	key another model holds) the stats count as lacking until it is built or gone. New ones are built in batches of that budget:
	a batch's builds share no scratch, the next batch takes the same bytes after a UAV barrier. The sizes are read
	once Waited() passes their list, never by polling, so two runs record the same commands. Skinned instances are
	skinned and built again every frame (PREFER_FAST_BUILD). The TLAS, the instance table and the skinned vertices
	hold until the next build.
*/

#include <map>
#include <set>
#include <span>

#include "d3d12_Buffers.h"
#include "d3d12_GpuScene.h"
#include "d3d12_HlslCompiler.h"
#include "d3d12_Pipelines.h"
#include "d3d12_RenderGraph.h"
#include "d3d12_Timers.h"

// The instance table, by the TLAS's InstanceIndex: what a hit needs beside the geometry records.
struct d3d12RtInstance_t {
	static constexpr uint32			NOT_SKINNED = 0xFFFFFFFF;
	uint32							firstGeometry;
	uint32							numGeometries;
	uint32							skinnedOffset;		// bytes of its first vertex in the skinned vertices, or NOT_SKINNED
	uint32							sceneInstance;		// its index in the update's instances
};
static_assert( sizeof( d3d12RtInstance_t ) == 16 );

class idD3D12RayTracing {
public:
	static constexpr uint64			UNUSED_BUILDS = 600;
	static constexpr uint64			SCRATCH_BUDGET = 32ull << 20;	// of a batch of new static BLAS, and of a delta's build
	static constexpr int			MAX_SIZES = 4096;				// compacted sizes in flight
	static constexpr int			MAX_COMPACTIONS = 256;			// per build: each makes a buffer

									idD3D12RayTracing( const idD3D12Device & device, idD3D12Submitter & submitter, idD3D12Graveyard & graveyard,
										idD3D12DescriptorPool & srvPool, idD3D12Barriers & barriers, idD3D12Uploader & uploader,
										idD3D12BufferStore & buffers, uint64 unusedBuilds = UNUSED_BUILDS, uint64 scratchBudget = SCRATCH_BUDGET );
									idD3D12RayTracing( const idD3D12RayTracing & ) = delete;
	idD3D12RayTracing &				operator=( const idD3D12RayTracing & ) = delete;
	void							CreateDefaults( const d3d12HlslStage_t & skinStage, const idD3D12ComputeRootSignature & computeRoot );

	// After an applied update, in a frame: the passes above, executed. False: nothing may be traced this frame. The
	// same update again builds nothing and keeps what the last build made.
	[[nodiscard]] bool				Build( idD3D12RenderGraph & graph, const idD3D12GpuScene & scene, idD3D12DebugGroups * groups,
										gpuSceneStats_t & stats );
	void							Invalidate() { valid = false; }		// a refused update
	[[nodiscard]] bool				Valid() const { return valid; }

	// The last build's, while Valid: views for ResourceDescriptorHeap, the buffers for the graph's imports.
	[[nodiscard]] d3d12Descriptor_t	TlasSrv() const { return tlasSrv; }
	[[nodiscard]] d3d12Descriptor_t	InstanceTableSrv() const { return tableSrv; }		// raw, d3d12RtInstance_t
	[[nodiscard]] d3d12Descriptor_t	SkinnedVerticesSrv() const { return skinnedSrv; }	// raw, idDrawVert
	[[nodiscard]] d3d12Descriptor_t	StaticVerticesSrv() const { return staticVertexSrv; }	// raw, of the scene's static buffers
	[[nodiscard]] d3d12Descriptor_t	StaticIndexesSrv() const { return staticIndexSrv; }
	[[nodiscard]] ID3D12Resource *	Tlas() const { return tlas.Get(); }
	[[nodiscard]] d3d12ResourceState_t &	TlasState() { return tlasState; }
	[[nodiscard]] ID3D12Resource *	InstanceTable() const { return table.Get(); }
	[[nodiscard]] d3d12ResourceState_t &	InstanceTableState() { return tableState; }
	[[nodiscard]] ID3D12Resource *	SkinnedVertices() const { return skinned.Get(); }
	[[nodiscard]] d3d12ResourceState_t &	SkinnedVerticesState() { return skinnedState; }
	[[nodiscard]] std::span<const d3d12RtInstance_t>	Instances() const { return instances; }
	[[nodiscard]] int				NumBlas() const { return static_cast<int>( cache.size() ); }
	[[nodiscard]] int				NumUncompacted() const {	// static BLAS not yet at their compacted size
		return static_cast<int>( std::ranges::count_if( cache, []( const auto & entry ) { return !entry.second.compacted; } ) );
	}

private:
	// What a static BLAS was built from: two keys that collide never share one.
	struct blasGeometry_t {
		uint32						vertexOffset;
		uint32						indexOffset;
		uint32						numIndexes;
		uint32						numVerts;
		bool						opaque;
		constexpr bool				operator==( const blasGeometry_t & ) const = default;
	};
	struct blas_t {
		std::vector<blasGeometry_t>	geometries;
		idComPtr<ID3D12Resource>	data;
		d3d12ResourceState_t		state;			// RAYTRACING_ACCELERATION_STRUCTURE for good
		uint64						bytes = 0;
		uint64						lastUsed = 0;	// the build counter
		uint64						builtAt = 0;	// the build that made it: a size read back must be of this one
		uint64						compactedBytes = 0;	// read back, not yet copied
		bool						asked = false;	// its size is in flight
		bool						compacted = false;	// or not worth it
	};
	struct pendingSize_t {
		uint64						key;
		uint64						builtAt;
		int							slot;
	};
	// One BuildRaytracingAccelerationStructure of the frame; the inputs point into geometryDescs.
	struct build_t {
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS	inputs = {};
		size_t						firstDesc = 0;
		ID3D12Resource *			target = nullptr;
		uint64						targetOffset = 0;
		uint64						scratchOffset = 0;
		uint32						batch = 0;		// builds of two batches may take the same scratch
	};
	struct skin_t {
		uint32						constants[8];	// rtSkin's b1
		uint32						numVerts;
	};
	struct compaction_t {
		blas_t *					entry;
		idComPtr<ID3D12Resource>	source;
		d3d12ResourceState_t		sourceState;
	};
	// Where an instance's BLAS lies once the frame's buffers are placed.
	struct placed_t {
		const idComPtr<ID3D12Resource> *	buffer = nullptr;
		uint64						offset = 0;
	};

	[[nodiscard]] bool				Reserve( idComPtr<ID3D12Resource> & buffer, d3d12ResourceState_t & state, uint64 & capacity, uint64 bytes,
										D3D12_RESOURCE_STATES initial, const wchar_t * name );
	[[nodiscard]] d3d12Descriptor_t	BufferSrv( bufferHandle_t handle );
	void							DropCache();
	void							ReadCompactedSizes();
	void							Compact();
	void							Evict();
	void							AskSizes();
	[[nodiscard]] uint64			TakeScratch( uint64 bytes );
	[[nodiscard]] bool				PlaceBuffers( uint64 skinnedBytes, uint64 skinnedBlasBytes, uint64 tlasBytes );
	void							Abandon();
	void							AddPasses( idD3D12RenderGraph & graph );

	const idD3D12Device &			device;
	idD3D12Submitter &				submitter;
	idD3D12Graveyard &				graveyard;
	idD3D12DescriptorPool &			srvPool;
	idD3D12Barriers &				barriers;
	idD3D12Uploader &				uploader;
	idD3D12BufferStore &			buffers;
	const uint64					unusedBuilds;
	const uint64					scratchBudget;
	d3d12ComputePipeline_t			skinPipeline;

	std::map<uint64, blas_t>		cache;			// by blasKey: ordered, so two runs walk it alike
	int								generation = -1;
	uint64							builds = 0;
	uint32							builtScene = 0;
	uint32							builtSequence = 0;
	bool							valid = false;
	bool							fullPending = false;	// a full update whose build did not finish: the next one builds all
	std::set<uint64>				lacking;		// the static BLAS keys the last full build left waiting
	std::vector<std::pair<bufferHandle_t, d3d12Descriptor_t>>	bufferSrvs;	// raw views of the buffers rtSkin and the hits read
	d3d12Descriptor_t				staticVertexSrv;
	d3d12Descriptor_t				staticIndexSrv;

	// the frame's work, kept until the next build: the graph records it in Execute
	std::vector<D3D12_RAYTRACING_GEOMETRY_DESC>	geometryDescs;
	std::vector<build_t>			blasBuilds;
	std::vector<blas_t *>			built;			// the static BLAS of blasBuilds
	std::vector<skin_t>				skins;
	std::vector<compaction_t>		compactions;
	std::vector<blas_t *>			asking;			// by size slot
	std::vector<D3D12_RAYTRACING_INSTANCE_DESC>	instanceDescs;
	std::vector<d3d12RtInstance_t>	instances;
	build_t							tlasBuild;
	uint64							scratchBytes = 0;	// taken this frame
	std::vector<pendingSize_t>		pending;		// sizes in the readback, written by the list of pendingFence
	uint64							pendingFence = 0;
	gpuSceneStats_t					lastStats;

	// persistent buffers: they only grow, the old one through the graveyard
	idComPtr<ID3D12Resource>		scratch;
	d3d12ResourceState_t			scratchState;
	uint64							scratchCapacity = 0;
	idComPtr<ID3D12Resource>		skinned;		// the skinned vertices
	d3d12ResourceState_t			skinnedState;
	uint64							skinnedCapacity = 0;
	d3d12Descriptor_t				skinnedSrv;
	d3d12Descriptor_t				skinnedUav;
	idComPtr<ID3D12Resource>		skinnedBlas;	// every skinned BLAS of the frame, 256-aligned
	d3d12ResourceState_t			skinnedBlasState;
	uint64							skinnedBlasCapacity = 0;
	idComPtr<ID3D12Resource>		tlas;
	d3d12ResourceState_t			tlasState;
	uint64							tlasCapacity = 0;
	d3d12Descriptor_t				tlasSrv;
	idComPtr<ID3D12Resource>		table;
	d3d12ResourceState_t			tableState;
	uint64							tableCapacity = 0;
	d3d12Descriptor_t				tableSrv;
	idComPtr<ID3D12Resource>		sizes;			// the compacted sizes, UAV
	d3d12ResourceState_t			sizesState;
	idComPtr<ID3D12Resource>		sizesReadback;
};
