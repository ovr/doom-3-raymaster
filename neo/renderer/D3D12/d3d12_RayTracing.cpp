// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#include "../../idlib/precompiled.h"

#include <bit>
#include <unordered_map>

#include "../tr_local.h"
#include "d3d12_RayTracing.h"

namespace {

constexpr uint64 AS_ALIGN = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;

[[nodiscard]] constexpr uint64 Align( uint64 bytes, uint64 alignment = AS_ALIGN ) {
	return ( bytes + alignment - 1 ) & ~( alignment - 1 );
}

[[nodiscard]] D3D12_SHADER_RESOURCE_VIEW_DESC RawSrv( uint64 bytes ) {
	D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
	view.Format = DXGI_FORMAT_R32_TYPELESS;
	view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	view.Buffer.NumElements = static_cast<UINT>( bytes / 4 );
	view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
	return view;
}

[[nodiscard]] D3D12_UNORDERED_ACCESS_VIEW_DESC RawUav( uint64 bytes ) {
	D3D12_UNORDERED_ACCESS_VIEW_DESC view = {};
	view.Format = DXGI_FORMAT_R32_TYPELESS;
	view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	view.Buffer.NumElements = static_cast<UINT>( bytes / 4 );
	view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
	return view;
}

// The triangles every ray takes at once: no alpha test, and a shadow caster, since the shadow rays look at the rest
// (a lamp's noshadows model around its light).
[[nodiscard]] bool BlasOpaque( const gpuSceneGeometry_t & record ) {
	constexpr uint32 BOTH = gpuSceneGeometry_t::GEO_OPAQUE | gpuSceneGeometry_t::GEO_CASTS_SHADOW;
	return ( record.flags & BOTH ) == BOTH;
}

// vertices: the geometry's first idDrawVert.
[[nodiscard]] D3D12_RAYTRACING_GEOMETRY_DESC Triangles( const gpuSceneGeometry_t & record, D3D12_GPU_VIRTUAL_ADDRESS vertices,
	D3D12_GPU_VIRTUAL_ADDRESS indexes ) {
	D3D12_RAYTRACING_GEOMETRY_DESC desc = {};
	desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
	desc.Flags = BlasOpaque( record ) ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
	desc.Triangles.IndexFormat = DXGI_FORMAT_R16_UINT;
	desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
	desc.Triangles.IndexCount = record.numIndexes;
	desc.Triangles.VertexCount = record.numVerts;
	desc.Triangles.IndexBuffer = indexes + record.indexOffset;
	desc.Triangles.VertexBuffer = { vertices + DRAWVERT_XYZ_OFFSET, sizeof( idDrawVert ) };
	return desc;
}

}	// namespace

idD3D12RayTracing::idD3D12RayTracing( const idD3D12Device & owner, idD3D12Submitter & submit, idD3D12Graveyard & graves,
	idD3D12DescriptorPool & srvs, idD3D12Barriers & barrierBatch, idD3D12Uploader & copies, idD3D12BufferStore & bufferStore, uint64 unused,
	uint64 budget ) :
	device( owner ),
	submitter( submit ),
	graveyard( graves ),
	srvPool( srvs ),
	barriers( barrierBatch ),
	uploader( copies ),
	buffers( bufferStore ),
	unusedBuilds( unused ),
	scratchBudget( budget ) {
	scratchState.decays = true;
	skinnedState.decays = true;
	tableState.decays = true;
	sizesState.decays = true;
	skinnedBlasState.current = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
	tlasState.current = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
}

void idD3D12RayTracing::CreateDefaults( const d3d12HlslStage_t & skinStage, const idD3D12ComputeRootSignature & computeRoot ) {
	skinPipeline = D3D12_CreateComputePipeline( device, computeRoot, skinStage.code, skinStage.reflection, L"doom3 rt skin" );
	sizes = D3D12_CreateBuffer( device, D3D12_HEAP_TYPE_DEFAULT, MAX_SIZES * sizeof( uint64 ), D3D12_RESOURCE_STATE_COMMON, L"doom3 rt blas sizes",
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS );
	sizesReadback = D3D12_CreateBuffer( device, D3D12_HEAP_TYPE_READBACK, MAX_SIZES * sizeof( uint64 ), D3D12_RESOURCE_STATE_COPY_DEST,
		L"doom3 rt blas sizes readback" );
}

bool idD3D12RayTracing::Reserve( idComPtr<ID3D12Resource> & buffer, d3d12ResourceState_t & state, uint64 & capacity, uint64 bytes,
	D3D12_RESOURCE_STATES initial, const wchar_t * name ) {
	if ( buffer && bytes <= capacity ) {
		return true;
	}
	// powers of two, so that a scene growing a little each frame does not make a buffer each frame
	const uint64 grown = Max( std::bit_ceil( Max( bytes, AS_ALIGN ) ), capacity * 2 );
	idComPtr<ID3D12Resource> made = D3D12_CreateBuffer( device, D3D12_HEAP_TYPE_DEFAULT, grown, initial, name, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS );
	if ( !made ) {
		return false;
	}
	if ( buffer ) {
		barriers.Flush();	// the batch may still name the old state
		graveyard.Bury( std::move( buffer ) );
	}
	d3d12ResourceState_t fresh;
	fresh.decays = state.decays;
	fresh.current = initial;
	state = fresh;
	buffer = std::move( made );
	capacity = grown;
	return true;
}

// A raw view per buffer rtSkin reads; the views of dead buffers go to the graveyard.
d3d12Descriptor_t idD3D12RayTracing::BufferSrv( bufferHandle_t handle ) {
	for ( size_t i = 0; i < bufferSrvs.size(); ) {
		if ( buffers.Find( bufferSrvs[i].first ) == nullptr ) {
			graveyard.Bury( srvPool, bufferSrvs[i].second );
			bufferSrvs.erase( bufferSrvs.begin() + static_cast<ptrdiff_t>( i ) );
		} else if ( bufferSrvs[i].first == handle ) {
			return bufferSrvs[i].second;
		} else {
			i++;
		}
	}
	const d3d12Buffer_t * buffer = buffers.Find( handle );
	if ( buffer == nullptr ) {
		return {};
	}
	const d3d12Descriptor_t slot = srvPool.Allocate();
	srvPool.CreateSrv( slot, *buffer->resource.Get(), RawSrv( buffer->bytes ) );
	bufferSrvs.emplace_back( handle, slot );
	return slot;
}

void idD3D12RayTracing::DropCache() {
	for ( auto & [key, entry] : cache ) {
		graveyard.Bury( std::move( entry.data ) );
	}
	cache.clear();
	pending.clear();
}

// The sizes of a list Waited() has passed; an entry built again since takes none of the old one's.
void idD3D12RayTracing::ReadCompactedSizes() {
	if ( pending.empty() || submitter.WaitedFence() < pendingFence ) {
		return;
	}
	void * mapped = nullptr;
	const D3D12_RANGE everything = { 0, MAX_SIZES * sizeof( uint64 ) };
	// a failed map answers nothing: its entries are asked again
	const uint64 * read = device.Check( sizesReadback->Map( 0, &everything, &mapped ), "Map (rt blas sizes)" ) ? static_cast<const uint64 *>( mapped ) : nullptr;
	for ( const pendingSize_t & size : pending ) {
		auto found = cache.find( size.key );
		if ( found == cache.end() || found->second.builtAt != size.builtAt ) {
			continue;
		}
		blas_t & entry = found->second;
		entry.asked = false;
		if ( read == nullptr ) {
			continue;
		}
		if ( read[size.slot] > 0 && Align( read[size.slot] ) < entry.bytes ) {
			entry.compactedBytes = read[size.slot];
		} else {
			entry.compacted = true;		// nothing to gain
		}
	}
	if ( read != nullptr ) {
		const D3D12_RANGE nothingWritten = { 0, 0 };
		sizesReadback->Unmap( 0, &nothingWritten );
	}
	pending.clear();
}

void idD3D12RayTracing::Compact() {
	for ( auto & [key, entry] : cache ) {
		if ( static_cast<int>( compactions.size() ) >= MAX_COMPACTIONS ) {
			break;
		}
		if ( entry.compacted || entry.compactedBytes == 0 ) {
			continue;
		}
		const uint64 bytes = Align( entry.compactedBytes );
		idComPtr<ID3D12Resource> made = D3D12_CreateBuffer( device, D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
			L"doom3 rt blas", D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS );
		if ( !made ) {
			return;
		}
		compaction_t & compaction = compactions.emplace_back();
		compaction.entry = &entry;
		compaction.source = std::move( entry.data );
		compaction.sourceState.current = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
		entry.data = std::move( made );
		entry.bytes = bytes;
		entry.compacted = true;
		entry.compactedBytes = 0;
	}
}

void idD3D12RayTracing::Evict() {
	for ( auto it = cache.begin(); it != cache.end(); ) {
		if ( builds - it->second.lastUsed > unusedBuilds ) {
			graveyard.Bury( std::move( it->second.data ) );
			it = cache.erase( it );
		} else {
			++it;
		}
	}
}

// One batch in flight: the static BLAS whose size nobody asked for yet, this build's among them.
void idD3D12RayTracing::AskSizes() {
	if ( !pending.empty() ) {
		return;
	}
	for ( auto & [key, entry] : cache ) {
		if ( static_cast<int>( asking.size() ) >= MAX_SIZES ) {
			break;
		}
		if ( !entry.asked && !entry.compacted && entry.compactedBytes == 0 ) {
			entry.asked = true;
			pending.push_back( { key, entry.builtAt, static_cast<int>( asking.size() ) } );
			asking.push_back( &entry );
		}
	}
}

uint64 idD3D12RayTracing::TakeScratch( uint64 bytes ) {
	const uint64 offset = Align( scratchBytes );
	scratchBytes = offset + Align( Max( bytes, 1ull ) );
	return offset;
}

// The frame's buffers at their sizes, and their views when one was made again.
bool idD3D12RayTracing::PlaceBuffers( uint64 skinnedBytes, uint64 skinnedBlasBytes, uint64 tlasBytes ) {
	ID3D12Resource * const oldSkinned = skinned.Get();
	ID3D12Resource * const oldTlas = tlas.Get();
	ID3D12Resource * const oldTable = table.Get();
	const uint64 tableBytes = Max<uint64>( instances.size(), 1 ) * sizeof( d3d12RtInstance_t );
	if ( !Reserve( scratch, scratchState, scratchCapacity, scratchBytes, D3D12_RESOURCE_STATE_COMMON, L"doom3 rt scratch" )
			|| !Reserve( skinned, skinnedState, skinnedCapacity, Max( skinnedBytes, 4ull ), D3D12_RESOURCE_STATE_COMMON, L"doom3 rt skinned vertices" )
			|| !Reserve( skinnedBlas, skinnedBlasState, skinnedBlasCapacity, skinnedBlasBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
				L"doom3 rt skinned blas" )
			|| !Reserve( tlas, tlasState, tlasCapacity, tlasBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"doom3 rt tlas" )
			|| !Reserve( table, tableState, tableCapacity, tableBytes, D3D12_RESOURCE_STATE_COMMON, L"doom3 rt instance table" ) ) {
		return false;
	}
	if ( skinned.Get() != oldSkinned ) {
		for ( d3d12Descriptor_t * view : { &skinnedSrv, &skinnedUav } ) {
			if ( view->IsValid() ) {
				graveyard.Bury( srvPool, *view );
			}
		}
		skinnedSrv = srvPool.Allocate();
		srvPool.CreateSrv( skinnedSrv, *skinned.Get(), RawSrv( skinnedCapacity ) );
		skinnedUav = srvPool.Allocate();
		srvPool.CreateUav( skinnedUav, *skinned.Get(), RawUav( skinnedCapacity ) );
	}
	if ( tlas.Get() != oldTlas ) {
		if ( tlasSrv.IsValid() ) {
			graveyard.Bury( srvPool, tlasSrv );
		}
		tlasSrv = srvPool.Allocate();
		srvPool.CreateAccelerationStructureSrv( tlasSrv, tlas->GetGPUVirtualAddress() );
	}
	if ( table.Get() != oldTable ) {
		if ( tableSrv.IsValid() ) {
			graveyard.Bury( srvPool, tableSrv );
		}
		tableSrv = srvPool.Allocate();
		srvPool.CreateSrv( tableSrv, *table.Get(), RawSrv( tableCapacity ) );
	}
	return true;
}

// A build that stops after the cache changed: its new entries hold no structure and the compactions none yet.
void idD3D12RayTracing::Abandon() {
	for ( compaction_t & compaction : compactions ) {
		graveyard.Bury( std::move( compaction.source ) );
	}
	compactions.clear();
	asking.clear();
	DropCache();
}

bool idD3D12RayTracing::Build( idD3D12RenderGraph & graph, const idD3D12GpuScene & scene, idD3D12DebugGroups * groups, gpuSceneStats_t & stats ) {
	if ( device.IsLost() || !scene.Valid() || !skinPipeline || !sizes || !sizesReadback ) {
		valid = false;
		stats = {};
		return false;
	}
	if ( valid && scene.SceneId() == builtScene && scene.Sequence() == builtSequence ) {
		stats = lastStats;
		return true;
	}
	// a full update whose build does not finish leaves its work to the next one
	fullPending = fullPending || scene.LastStats().full;
	valid = false;
	stats = {};
	builds++;
	if ( scene.StaticGeneration() != generation ) {
		DropCache();
		generation = scene.StaticGeneration();
	}
	const d3d12Buffer_t * vertexBuffer = buffers.Use( scene.StaticVertexBuffer() );
	const d3d12Buffer_t * indexBuffer = buffers.Use( scene.StaticIndexBuffer() );
	const d3d12Descriptor_t vertexSrv = BufferSrv( scene.StaticVertexBuffer() );
	const d3d12Descriptor_t indexSrv = BufferSrv( scene.StaticIndexBuffer() );
	if ( vertexBuffer == nullptr || indexBuffer == nullptr || !vertexSrv.IsValid() || !indexSrv.IsValid() ) {
		return false;
	}
	staticVertexSrv = vertexSrv;
	staticIndexSrv = indexSrv;

	geometryDescs.clear();
	blasBuilds.clear();
	built.clear();
	skins.clear();
	compactions.clear();
	asking.clear();
	instanceDescs.clear();
	instances.clear();
	scratchBytes = 0;
	ReadCompactedSizes();
	Evict();		// before anything of this build points at an entry
	Compact();

	const std::span<const gpuSceneGeometry_t> geometries = scene.Geometries();
	const std::span<const gpuSceneInstance_t> sceneInstances = scene.Instances();
	size_t maxDescs = 0;
	for ( const gpuSceneInstance_t & instance : sceneInstances ) {
		maxDescs += instance.numGeometries;
	}
	geometryDescs.reserve( maxDescs );		// the inputs point into it: it must not move
	std::vector<placed_t> placed( sceneInstances.size() );
	std::vector<uint32> skinnedOffsets( sceneInstances.size(), d3d12RtInstance_t::NOT_SKINNED );
	// A full update builds every structure it lacks, so the frame traces all its casters; a delta up to the budget.
	// The builds go in batches of the budget: one batch's run side by side, the next takes the same scratch.
	const bool all = fullPending;
	// what the full update left waiting, until it is built or gone: the frames lack casters they count on
	std::set<uint64> stillLacking;
	const auto wait = [&]( uint64 key ) {
		stats.waiting++;
		if ( all || lacking.contains( key ) ) {
			stats.lacking++;
			if ( key != 0 ) {
				stillLacking.insert( key );
			}
		}
	};
	uint64 staticScratch = 0;		// of the build's new static BLAS
	uint64 batchScratch = 0;		// of the batch being filled
	uint64 batchRegion = 0;			// the largest batch
	uint32 batch = 0;
	uint64 skinnedBytes = 0;
	uint64 skinnedBlasBytes = 0;

	for ( size_t i = 0; i < sceneInstances.size(); i++ ) {
		const gpuSceneInstance_t & instance = sceneInstances[i];
		const size_t first = instance.firstGeometry;
		if ( instance.numGeometries == 0 || first + instance.numGeometries > geometries.size() ) {
			continue;
		}
		const std::span<const gpuSceneGeometry_t> records = geometries.subspan( first, instance.numGeometries );
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
		inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
		inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		inputs.NumDescs = instance.numGeometries;

		if ( instance.blasKey == 0 ) {
			// skinned: this frame's vertices, a BLAS of its own
			const d3d12Descriptor_t jointSrv = BufferSrv( instance.jointBuffer );
			if ( instance.skinnedVerts == 0 || !jointSrv.IsValid() || buffers.Use( instance.jointBuffer ) == nullptr ) {
				wait( instance.blasKey );
				continue;
			}
			const uint64 base = skinnedBytes;
			skinnedBytes += static_cast<uint64>( instance.skinnedVerts ) * sizeof( idDrawVert );
			const size_t firstDesc = geometryDescs.size();
			for ( const gpuSceneGeometry_t & record : records ) {
				// relative to the skinned vertices: placed once their buffer is
				const uint32 target = static_cast<uint32>( base ) + record.skinnedOffset;
				geometryDescs.push_back( Triangles( record, target, indexBuffer->gpu ) );
				skin_t & skin = skins.emplace_back();
				const uint32 constants[8] = { vertexSrv.index, record.vertexOffset, record.numVerts, jointSrv.index,
					static_cast<uint32>( instance.jointOffset ), 0, target, 0 };
				memcpy( skin.constants, constants, sizeof( constants ) );
				skin.numVerts = record.numVerts;
			}
			inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
			inputs.pGeometryDescs = &geometryDescs[firstDesc];
			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
			device.Get().GetRaytracingAccelerationStructurePrebuildInfo( &inputs, &info );
			build_t & build = blasBuilds.emplace_back();
			build.inputs = inputs;
			build.firstDesc = firstDesc;
			build.targetOffset = Align( skinnedBlasBytes );
			build.scratchOffset = TakeScratch( info.ScratchDataSizeInBytes );	// after the batches' region, once that is known
			build.batch = batch;
			skinnedBlasBytes = build.targetOffset + Align( info.ResultDataMaxSizeInBytes );
			placed[i] = { std::addressof( skinnedBlas ), build.targetOffset };	// ComPtr's & releases it
			skinnedOffsets[i] = static_cast<uint32>( base );
			stats.skinned++;
			stats.skinnedVerts += static_cast<int>( instance.skinnedVerts );
			continue;
		}

		auto found = cache.find( instance.blasKey );
		if ( found != cache.end() ) {
			const std::vector<blasGeometry_t> & madeFrom = found->second.geometries;
			bool same = madeFrom.size() == records.size();
			for ( size_t g = 0; same && g < records.size(); g++ ) {
				same = madeFrom[g] == blasGeometry_t{ records[g].vertexOffset, records[g].indexOffset, records[g].numIndexes, records[g].numVerts,
					BlasOpaque( records[g] ) };
			}
			if ( !same ) {
				wait( instance.blasKey );		// two models with one key: the second waits for the first to go
				continue;
			}
			found->second.lastUsed = builds;
			placed[i] = { std::addressof( found->second.data ), 0 };
			continue;
		}
		if ( !all && staticScratch >= scratchBudget ) {
			wait( instance.blasKey );
			continue;
		}
		blas_t entry;
		const size_t firstDesc = geometryDescs.size();
		for ( const gpuSceneGeometry_t & record : records ) {
			geometryDescs.push_back( Triangles( record, vertexBuffer->gpu + record.vertexOffset, indexBuffer->gpu ) );
			entry.geometries.push_back( { record.vertexOffset, record.indexOffset, record.numIndexes, record.numVerts,
				BlasOpaque( record ) } );
		}
		inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
		inputs.pGeometryDescs = &geometryDescs[firstDesc];
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
		device.Get().GetRaytracingAccelerationStructurePrebuildInfo( &inputs, &info );
		entry.bytes = Align( info.ResultDataMaxSizeInBytes );
		entry.data = D3D12_CreateBuffer( device, D3D12_HEAP_TYPE_DEFAULT, entry.bytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
			L"doom3 rt blas", D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS );
		if ( !entry.data ) {
			geometryDescs.resize( firstDesc );
			wait( instance.blasKey );
			continue;
		}
		entry.state.current = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
		entry.lastUsed = builds;
		entry.builtAt = builds;
		blas_t & made = cache.emplace( instance.blasKey, std::move( entry ) ).first->second;
		build_t & build = blasBuilds.emplace_back();
		build.inputs = inputs;
		build.firstDesc = firstDesc;
		build.target = made.data.Get();
		const uint64 scratchNeed = Align( Max<uint64>( info.ScratchDataSizeInBytes, 1 ) );
		if ( batchScratch > 0 && batchScratch + scratchNeed > scratchBudget ) {
			batch++;
			batchScratch = 0;
		}
		build.scratchOffset = batchScratch;
		build.batch = batch;
		batchScratch += scratchNeed;
		batchRegion = Max( batchRegion, batchScratch );
		staticScratch += scratchNeed;
		built.push_back( &made );
		placed[i] = { std::addressof( made.data ), 0 };
		stats.built++;
	}
	AskSizes();

	// the scratch of the skinned builds and of the TLAS lies after the batches' region
	for ( build_t & build : blasBuilds ) {
		if ( build.target == nullptr ) {
			build.scratchOffset += batchRegion;
		}
	}
	scratchBytes += batchRegion;

	// the TLAS of every placed instance
	for ( size_t i = 0; i < sceneInstances.size(); i++ ) {
		if ( placed[i].buffer != nullptr ) {
			const gpuSceneInstance_t & instance = sceneInstances[i];
			instances.push_back( { instance.firstGeometry, instance.numGeometries, skinnedOffsets[i], static_cast<uint32>( i ) } );
		}
	}
	tlasBuild = {};
	tlasBuild.inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
	tlasBuild.inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
	tlasBuild.inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	tlasBuild.inputs.NumDescs = static_cast<UINT>( instances.size() );
	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasInfo = {};
	device.Get().GetRaytracingAccelerationStructurePrebuildInfo( &tlasBuild.inputs, &tlasInfo );
	tlasBuild.scratchOffset = TakeScratch( tlasInfo.ScratchDataSizeInBytes );
	if ( !PlaceBuffers( skinnedBytes, skinnedBlasBytes, Align( tlasInfo.ResultDataMaxSizeInBytes ) ) ) {
		Abandon();
		return false;
	}

	// what the buffers' places decide: the skinned geometry and BLAS, the instances' structures
	const D3D12_GPU_VIRTUAL_ADDRESS skinnedAddress = skinned->GetGPUVirtualAddress();
	for ( build_t & build : blasBuilds ) {
		if ( build.target == nullptr ) {
			build.target = skinnedBlas.Get();
			for ( UINT d = 0; d < build.inputs.NumDescs; d++ ) {
				geometryDescs[build.firstDesc + d].Triangles.VertexBuffer.StartAddress += skinnedAddress;
			}
		}
	}
	for ( skin_t & skin : skins ) {
		skin.constants[5] = skinnedUav.index;
	}
	for ( const d3d12RtInstance_t & entry : instances ) {
		const gpuSceneInstance_t & instance = sceneInstances[entry.sceneInstance];
		D3D12_RAYTRACING_INSTANCE_DESC & desc = instanceDescs.emplace_back();
		memset( &desc, 0, sizeof( desc ) );
		memcpy( desc.Transform, instance.transform, sizeof( desc.Transform ) );
		desc.InstanceID = instance.firstGeometry & 0xFFFFFF;
		desc.InstanceMask = instance.mask & 0xFF;
		// Doom 3 draws the side that winds clockwise in GL's window: DXR's back face, unless flipped. The rays cull
		// back faces, and an instance culls as a whole: its geometries are all two-sided or none is (R15)
		desc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE;
		const std::span<const gpuSceneGeometry_t> records = geometries.subspan( instance.firstGeometry, instance.numGeometries );
		const auto twoSided = []( const gpuSceneGeometry_t & geometry ) { return ( geometry.flags & gpuSceneGeometry_t::GEO_TWO_SIDED ) != 0; };
		assert( std::ranges::all_of( records, twoSided ) || std::ranges::none_of( records, twoSided ) );
		if ( !records.empty() && twoSided( records.front() ) ) {
			desc.Flags |= D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
		}
		desc.AccelerationStructure = ( *placed[entry.sceneInstance].buffer )->GetGPUVirtualAddress() + placed[entry.sceneInstance].offset;
	}
	if ( !instances.empty() ) {
		uploader.BufferRange( *table.Get(), tableState, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, 0,
			std::span( reinterpret_cast<const byte *>( instances.data() ), instances.size() * sizeof( d3d12RtInstance_t ) ) );
	}

	AddPasses( graph );
	graph.Execute( groups );
	for ( compaction_t & compaction : compactions ) {
		graveyard.Bury( std::move( compaction.source ) );
	}
	if ( !asking.empty() ) {
		pendingFence = submitter.PendingFence();
	}

	stats.tlasInstances = static_cast<int>( instances.size() );
	stats.blas = static_cast<int>( cache.size() );
	stats.compacted = static_cast<int>( compactions.size() );
	for ( const auto & [key, entry] : cache ) {
		stats.blasBytes += entry.bytes;
	}
	stats.frameBytes = scratchCapacity + skinnedCapacity + skinnedBlasCapacity + tlasCapacity + tableCapacity;
	lacking = std::move( stillLacking );
	fullPending = false;
	valid = !device.IsLost();
	stats.traceable = valid;
	lastStats = stats;
	builtScene = scene.SceneId();
	builtSequence = scene.Sequence();
	return valid;
}

void idD3D12RayTracing::AddPasses( idD3D12RenderGraph & graph ) {
	using access = d3d12GraphAccess_t;
	using pass = d3d12GraphPass_t;
	const d3d12GraphResource_t scratchUse = graph.Import( "rt scratch", { scratch.Get(), &scratchState, {}, {}, {}, 0, 0 } );
	const d3d12GraphResource_t skinnedUse = graph.Import( "rt skinned vertices", { skinned.Get(), &skinnedState, skinnedSrv, skinnedUav, {}, 0, 0 } );
	const d3d12GraphResource_t skinnedBlasUse = graph.Import( "rt skinned blas", { skinnedBlas.Get(), &skinnedBlasState, {}, {}, {}, 0, 0 } );
	const d3d12GraphResource_t tlasUse = graph.Import( "rt tlas", { tlas.Get(), &tlasState, tlasSrv, {}, {}, 0, 0 } );
	const d3d12GraphResource_t sizesUse = graph.Import( "rt blas sizes", { sizes.Get(), &sizesState, {}, {}, {}, 0, 0 } );
	// one import per structure, so that the graph sees every hazard on it
	std::unordered_map<const blas_t *, d3d12GraphResource_t> imports;
	const auto Use = [&graph, &imports]( blas_t & entry ) {
		auto [it, inserted] = imports.try_emplace( &entry );
		if ( inserted ) {
			it->second = graph.Import( "rt blas", { entry.data.Get(), &entry.state, {}, {}, {}, 0, 0 } );
		}
		return it->second;
	};

	if ( !skins.empty() ) {
		graph.AddPass( "rt skin", pass::COMPUTE, { { skinnedUse, access::UAV } }, [this]( idD3D12GraphContext & context ) {
			for ( const skin_t & skin : skins ) {
				context.Dispatch( skinPipeline, nullptr, skin.numVerts, 1, 1, skin.constants );
			}
		} );
	}

	std::vector<d3d12GraphUse_t> tlasUses = { { tlasUse, access::AS_WRITE }, { scratchUse, access::UAV } };
	if ( !blasBuilds.empty() ) {
		std::vector<d3d12GraphUse_t> uses = { { scratchUse, access::UAV } };
		for ( blas_t * entry : built ) {
			uses.push_back( { Use( *entry ), access::AS_WRITE } );
			tlasUses.push_back( { Use( *entry ), access::AS_READ } );
		}
		if ( !skins.empty() ) {
			uses.push_back( { skinnedUse, access::SRV } );
			uses.push_back( { skinnedBlasUse, access::AS_WRITE } );
			tlasUses.push_back( { skinnedBlasUse, access::AS_READ } );
		}
		graph.AddPass( "rt blas", pass::AS_BUILD, uses, [this, scratchUse]( idD3D12GraphContext & context ) {
			uint32 batch = 0;
			for ( const build_t & build : blasBuilds ) {
				// the builds before are done with the scratch the next batch takes
				if ( build.batch != batch ) {
					context.UavBarrier( scratchUse );
					barriers.Flush();	// the graph's batch: a raw build flushes nothing
					batch = build.batch;
				}
				D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc = {};
				desc.Inputs = build.inputs;
				desc.DestAccelerationStructureData = build.target->GetGPUVirtualAddress() + build.targetOffset;
				desc.ScratchAccelerationStructureData = context.Address( scratchUse ) + build.scratchOffset;
				context.List().BuildRaytracingAccelerationStructure( &desc, 0, nullptr );
			}
		} );
	}

	if ( !compactions.empty() ) {
		std::vector<d3d12GraphUse_t> uses;
		std::vector<std::pair<d3d12GraphResource_t, d3d12GraphResource_t>> pairs;
		for ( compaction_t & compaction : compactions ) {
			const d3d12GraphResource_t source = graph.Import( "rt blas uncompacted", { compaction.source.Get(), &compaction.sourceState, {}, {}, {}, 0, 0 } );
			const d3d12GraphResource_t target = Use( *compaction.entry );
			uses.push_back( { source, access::AS_READ } );
			uses.push_back( { target, access::AS_WRITE } );
			tlasUses.push_back( { target, access::AS_READ } );
			pairs.emplace_back( source, target );
		}
		graph.AddPass( "rt blas compact", pass::AS_BUILD, uses, [pairs]( idD3D12GraphContext & context ) {
			for ( const auto & [source, target] : pairs ) {
				context.List().CopyRaytracingAccelerationStructure( context.Address( target ), context.Address( source ),
					D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT );
			}
		} );
	}

	graph.AddPass( "rt tlas", pass::AS_BUILD, tlasUses, [this, tlasUse, scratchUse]( idD3D12GraphContext & context ) {
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc = {};
		desc.Inputs = tlasBuild.inputs;
		if ( !instanceDescs.empty() ) {
			const d3d12UploadSpan_t descs = context.Parms( std::span( reinterpret_cast<const byte *>( instanceDescs.data() ),
				instanceDescs.size() * sizeof( D3D12_RAYTRACING_INSTANCE_DESC ) ) );
			desc.Inputs.InstanceDescs = descs.gpu;
		}
		desc.DestAccelerationStructureData = context.Address( tlasUse );
		desc.ScratchAccelerationStructureData = context.Address( scratchUse ) + tlasBuild.scratchOffset;
		context.List().BuildRaytracingAccelerationStructure( &desc, 0, nullptr );
	} );

	if ( !asking.empty() ) {
		std::vector<d3d12GraphUse_t> uses = { { sizesUse, access::UAV } };
		for ( blas_t * entry : asking ) {
			uses.push_back( { Use( *entry ), access::AS_READ } );
		}
		graph.AddPass( "rt blas sizes", pass::AS_BUILD, uses, [this, sizesUse]( idD3D12GraphContext & context ) {
			std::vector<D3D12_GPU_VIRTUAL_ADDRESS> sources;
			sources.reserve( asking.size() );
			for ( const blas_t * entry : asking ) {
				sources.push_back( entry->data->GetGPUVirtualAddress() );
			}
			const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC desc = { context.Address( sizesUse ),
				D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE };
			context.List().EmitRaytracingAccelerationStructurePostbuildInfo( &desc, static_cast<UINT>( sources.size() ), sources.data() );
		} );
		graph.AddPass( "rt blas readback", pass::COPY, { { sizesUse, access::COPY_SOURCE } }, [this, sizesUse]( idD3D12GraphContext & context ) {
			context.List().CopyBufferRegion( sizesReadback.Get(), 0, &context.Resource( sizesUse ), 0, asking.size() * sizeof( uint64 ) );
		}, true );
	}
}
