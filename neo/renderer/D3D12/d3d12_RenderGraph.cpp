// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "../../idlib/precompiled.h"

#include <string>

#include "../tr_local.h"
#include "d3d12_RenderGraph.h"

namespace {

constexpr uint64 PARM_ALIGN = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;

[[nodiscard]] std::wstring DebugName( const char * name ) {
	std::wstring wide = L"doom3 ";
	for ( const char * c = name; *c != '\0'; c++ ) {
		wide.push_back( static_cast<wchar_t>( static_cast<unsigned char>( *c ) ) );
	}
	return wide;
}

[[nodiscard]] D3D12_RESOURCE_DESC ResourceDesc( const d3d12PoolDesc_t & desc ) {
	D3D12_RESOURCE_DESC resource = {};
	resource.Width = desc.width;
	resource.Height = desc.height;
	resource.DepthOrArraySize = 1;
	resource.MipLevels = desc.mips;
	resource.Format = desc.format;
	resource.SampleDesc.Count = desc.samples;
	resource.Flags = ( desc.renderTarget ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE )
		| ( desc.unorderedAccess ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE );
	switch ( desc.kind ) {
		case d3d12PoolDesc_t::kind_t::TEXTURE_2D:
			resource.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			break;
		case d3d12PoolDesc_t::kind_t::TEXTURE_3D:
			resource.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
			resource.DepthOrArraySize = desc.depth;
			break;
		case d3d12PoolDesc_t::kind_t::ACCELERATION_STRUCTURE:
			resource.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
			[[fallthrough]];
		case d3d12PoolDesc_t::kind_t::BUFFER:
			resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			resource.Height = 1;
			resource.MipLevels = 1;
			resource.Format = DXGI_FORMAT_UNKNOWN;
			resource.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			break;
	}
	return resource;
}

// mip UINT16_MAX: every mip.
[[nodiscard]] D3D12_SHADER_RESOURCE_VIEW_DESC ShaderView( const d3d12PoolDesc_t & desc, uint16 mip ) {
	const bool all = ( mip == d3d12GraphUse_t::ALL_MIPS );
	D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
	view.Format = desc.format;
	view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	switch ( desc.kind ) {
		case d3d12PoolDesc_t::kind_t::TEXTURE_2D:
			if ( desc.samples > 1 ) {
				view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
				break;
			}
			view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			view.Texture2D.MostDetailedMip = all ? 0 : mip;
			view.Texture2D.MipLevels = all ? desc.mips : 1;
			break;
		case d3d12PoolDesc_t::kind_t::TEXTURE_3D:
			view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
			view.Texture3D.MostDetailedMip = all ? 0 : mip;
			view.Texture3D.MipLevels = all ? desc.mips : 1;
			break;
		case d3d12PoolDesc_t::kind_t::BUFFER:
		case d3d12PoolDesc_t::kind_t::ACCELERATION_STRUCTURE:	// its view is made by CreateViews
			view.Format = DXGI_FORMAT_R32_TYPELESS;
			view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			view.Buffer.NumElements = desc.width / 4;
			view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
			break;
	}
	return view;
}

[[nodiscard]] D3D12_UNORDERED_ACCESS_VIEW_DESC AccessView( const d3d12PoolDesc_t & desc, uint16 mip ) {
	D3D12_UNORDERED_ACCESS_VIEW_DESC view = {};
	view.Format = desc.format;
	switch ( desc.kind ) {
		case d3d12PoolDesc_t::kind_t::TEXTURE_2D:
			view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			view.Texture2D.MipSlice = mip;
			break;
		case d3d12PoolDesc_t::kind_t::TEXTURE_3D:
			view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
			view.Texture3D.MipSlice = mip;
			view.Texture3D.WSize = UINT_MAX;	// every slice of the mip
			break;
		case d3d12PoolDesc_t::kind_t::BUFFER:
		case d3d12PoolDesc_t::kind_t::ACCELERATION_STRUCTURE:	// never made
			view.Format = DXGI_FORMAT_R32_TYPELESS;
			view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
			view.Buffer.NumElements = desc.width / 4;
			view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
			break;
	}
	return view;
}

[[nodiscard]] constexpr D3D12_RESOURCE_STATES StateFor( d3d12GraphAccess_t access ) {
	switch ( access ) {
		case d3d12GraphAccess_t::SRV:			return D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
		case d3d12GraphAccess_t::UAV:			return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		case d3d12GraphAccess_t::RTV:			return D3D12_RESOURCE_STATE_RENDER_TARGET;
		case d3d12GraphAccess_t::COPY_SOURCE:	return D3D12_RESOURCE_STATE_COPY_SOURCE;
		case d3d12GraphAccess_t::COPY_DEST:		return D3D12_RESOURCE_STATE_COPY_DEST;
		case d3d12GraphAccess_t::DEPTH_READ:	return D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
		default:								return D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
	}
}

[[nodiscard]] constexpr bool Writes( d3d12GraphAccess_t access ) {
	return access == d3d12GraphAccess_t::UAV || access == d3d12GraphAccess_t::RTV || access == d3d12GraphAccess_t::COPY_DEST
		|| access == d3d12GraphAccess_t::AS_WRITE;
}

[[nodiscard]] constexpr bool Reads( d3d12GraphAccess_t access ) {
	return access == d3d12GraphAccess_t::SRV || access == d3d12GraphAccess_t::UAV || access == d3d12GraphAccess_t::COPY_SOURCE
		|| access == d3d12GraphAccess_t::AS_READ || access == d3d12GraphAccess_t::DEPTH_READ;
}

[[nodiscard]] constexpr bool IsStructureAccess( d3d12GraphAccess_t access ) {
	return access == d3d12GraphAccess_t::AS_READ || access == d3d12GraphAccess_t::AS_WRITE;
}

[[nodiscard]] constexpr uint32 Groups( uint32 threads, uint32 groupSize ) {
	return ( groupSize == 0 ) ? 0 : ( threads + groupSize - 1 ) / groupSize;
}

}	// namespace

// ---- the pool ----------------------------------------------------------------------------------------------

idD3D12ResourcePool::idD3D12ResourcePool( const idD3D12Device & owner, idD3D12Graveyard & graves, idD3D12DescriptorPool & srvs,
	idD3D12DescriptorPool & rtvs ) :
	device( owner ),
	graveyard( graves ),
	srvPool( srvs ),
	rtvPool( rtvs ) {
}

// The first free match in creation order, so two runs hand out the same resources.
d3d12PooledResource_t * idD3D12ResourcePool::Acquire( const d3d12PoolDesc_t & desc, const char * name ) {
	d3d12PooledResource_t * found = nullptr;
	for ( const std::unique_ptr<d3d12PooledResource_t> & entry : entries ) {
		if ( !entry->inUse && entry->desc == desc ) {
			found = entry.get();
			break;
		}
	}
	if ( found == nullptr ) {
		std::unique_ptr<d3d12PooledResource_t> made = Create( desc );
		if ( !made ) {
			return nullptr;
		}
		found = made.get();
		entries.push_back( std::move( made ) );
		created++;
	}
	if ( idStr::Cmp( found->name, name ) != 0 ) {
		found->name = name;
		found->resource->SetName( DebugName( name ).c_str() );
	}
	found->inUse = true;
	return found;
}

std::unique_ptr<d3d12PooledResource_t> idD3D12ResourcePool::Create( const d3d12PoolDesc_t & desc ) {
	const bool structure = ( desc.kind == d3d12PoolDesc_t::kind_t::ACCELERATION_STRUCTURE );
	assert( desc.mips >= 1 && ( desc.kind != d3d12PoolDesc_t::kind_t::BUFFER || ( desc.mips == 1 && desc.width % 4 == 0 ) ) );
	assert( !structure || ( desc.mips == 1 && desc.width % D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT == 0 ) );
	assert( !desc.renderTarget || desc.kind == d3d12PoolDesc_t::kind_t::TEXTURE_2D );
	assert( desc.samples == 1 || ( desc.kind == d3d12PoolDesc_t::kind_t::TEXTURE_2D && desc.mips == 1 && !desc.unorderedAccess ) );
	auto pooled = std::make_unique<d3d12PooledResource_t>();
	pooled->desc = desc;
	const D3D12_RESOURCE_DESC resourceDesc = ResourceDesc( desc );
	const D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
	const D3D12_RESOURCE_STATES initial = structure ? D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE : D3D12_RESOURCE_STATE_COMMON;
	if ( !device.Check( device.Get().CreateCommittedResource( &heap, D3D12_HEAP_FLAG_NONE, &resourceDesc, initial, nullptr,
			IID_PPV_ARGS( &pooled->resource ) ), "CreateCommittedResource (render graph)" ) ) {
		return nullptr;
	}
	pooled->bytes = device.Get().GetResourceAllocationInfo( 0, 1, &resourceDesc ).SizeInBytes;
	pooled->states.resize( desc.mips );
	pooled->states[0].current = initial;
	// buffers go back to COMMON after every list, as the runtime puts them
	pooled->states[0].decays = ( desc.kind == d3d12PoolDesc_t::kind_t::BUFFER );
	CreateViews( *pooled );
	bytes += pooled->bytes;
	return pooled;
}

void idD3D12ResourcePool::CreateViews( d3d12PooledResource_t & pooled ) {
	const d3d12PoolDesc_t & desc = pooled.desc;
	ID3D12Resource & resource = *pooled.resource.Get();
	pooled.srv = srvPool.Allocate();
	if ( desc.kind == d3d12PoolDesc_t::kind_t::ACCELERATION_STRUCTURE ) {
		srvPool.CreateAccelerationStructureSrv( pooled.srv, resource.GetGPUVirtualAddress() );
		return;
	}
	srvPool.CreateSrv( pooled.srv, resource, ShaderView( desc, d3d12GraphUse_t::ALL_MIPS ) );
	for ( uint16 mip = 0; mip < desc.mips; mip++ ) {
		if ( desc.mips > 1 ) {
			pooled.mipSrvs.push_back( srvPool.Allocate() );
			srvPool.CreateSrv( pooled.mipSrvs.back(), resource, ShaderView( desc, mip ) );
		}
		if ( desc.unorderedAccess ) {
			pooled.uavs.push_back( srvPool.Allocate() );
			srvPool.CreateUav( pooled.uavs.back(), resource, AccessView( desc, mip ) );
		}
		if ( desc.renderTarget ) {
			D3D12_RENDER_TARGET_VIEW_DESC view = {};
			view.Format = desc.format;
			view.ViewDimension = ( desc.samples > 1 ) ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
			view.Texture2D.MipSlice = mip;
			pooled.rtvs.push_back( rtvPool.Allocate() );
			device.Get().CreateRenderTargetView( &resource, &view, rtvPool.Cpu( pooled.rtvs.back() ) );
		}
	}
}

void idD3D12ResourcePool::Release( d3d12PooledResource_t & pooled ) {
	assert( pooled.inUse );
	pooled.inUse = false;
	pooled.lastUsed = frame;
}

void idD3D12ResourcePool::EndFrame() {
	std::erase_if( entries, [this]( std::unique_ptr<d3d12PooledResource_t> & entry ) {
		if ( entry->inUse || frame - entry->lastUsed < UNUSED_FRAMES ) {
			return false;
		}
		Free( *entry );
		return true;
	} );
	frame++;
	created = 0;
}

void idD3D12ResourcePool::FreeAll() {
	for ( std::unique_ptr<d3d12PooledResource_t> & entry : entries ) {
		assert( !entry->inUse );
		Free( *entry );
	}
	entries.clear();
}

// A list that read the views may still run: they wait in the graveyard with the resource.
void idD3D12ResourcePool::Free( d3d12PooledResource_t & pooled ) {
	bytes -= pooled.bytes;
	graveyard.Bury( std::move( pooled.resource ) );
	graveyard.Bury( srvPool, pooled.srv );
	for ( const std::vector<d3d12Descriptor_t> * views : { &pooled.mipSrvs, &pooled.uavs } ) {
		for ( const d3d12Descriptor_t slot : *views ) {
			graveyard.Bury( srvPool, slot );
		}
	}
	for ( const d3d12Descriptor_t slot : pooled.rtvs ) {
		graveyard.Bury( rtvPool, slot );
	}
}

// ---- the context of a pass ---------------------------------------------------------------------------------

ID3D12GraphicsCommandList4 & idD3D12GraphContext::List() {
	return graph.submitter.List();
}

void idD3D12GraphContext::CheckUse( [[maybe_unused]] d3d12GraphResource_t resource,
	[[maybe_unused]] std::initializer_list<d3d12GraphAccess_t> accesses ) const {
#ifdef _DEBUG
	const bool used = std::ranges::any_of( graph.Uses( graph.passes[pass] ), [resource, accesses]( const d3d12GraphUse_t & use ) {
		return use.resource == resource && std::ranges::find( accesses, use.access ) != accesses.end();
	} );
	assert( used );
#endif
}

ID3D12Resource & idD3D12GraphContext::Resource( d3d12GraphResource_t resource ) const {
	const idD3D12RenderGraph::resource_t & made = graph.resources[resource.index];
	return made.imported ? *made.import.resource : *made.pooled->resource.Get();
}

uint32 idD3D12GraphContext::Srv( d3d12GraphResource_t resource, uint16 mip ) const {
	CheckUse( resource, { d3d12GraphAccess_t::SRV, d3d12GraphAccess_t::AS_READ, d3d12GraphAccess_t::DEPTH_READ } );
	const idD3D12RenderGraph::resource_t & made = graph.resources[resource.index];
	if ( made.imported ) {
		return made.import.srv.index;
	}
	const d3d12PooledResource_t & pooled = *made.pooled;
	return ( mip == d3d12GraphUse_t::ALL_MIPS || pooled.mipSrvs.empty() ) ? pooled.srv.index : pooled.mipSrvs[mip].index;
}

uint32 idD3D12GraphContext::Uav( d3d12GraphResource_t resource, uint16 mip ) const {
	CheckUse( resource, { d3d12GraphAccess_t::UAV } );
	const idD3D12RenderGraph::resource_t & made = graph.resources[resource.index];
	return made.imported ? made.import.uav.index : made.pooled->uavs[mip].index;
}

D3D12_CPU_DESCRIPTOR_HANDLE idD3D12GraphContext::Rtv( d3d12GraphResource_t resource, uint16 mip ) const {
	CheckUse( resource, { d3d12GraphAccess_t::RTV } );
	const idD3D12RenderGraph::resource_t & made = graph.resources[resource.index];
	return made.imported ? made.import.rtv : graph.rtvPool.Cpu( made.pooled->rtvs[mip] );
}

uint32 idD3D12GraphContext::Width( d3d12GraphResource_t resource, uint16 mip ) const {
	const idD3D12RenderGraph::resource_t & made = graph.resources[resource.index];
	return made.imported ? made.import.width : made.desc.MipWidth( mip );
}

uint32 idD3D12GraphContext::Height( d3d12GraphResource_t resource, uint16 mip ) const {
	const idD3D12RenderGraph::resource_t & made = graph.resources[resource.index];
	return made.imported ? made.import.height : made.desc.MipHeight( mip );
}

d3d12UploadSpan_t idD3D12GraphContext::Parms( std::span<const byte> bytes ) {
	const uint64 size = Max<uint64>( ( bytes.size() + PARM_ALIGN - 1 ) & ~( PARM_ALIGN - 1 ), PARM_ALIGN );
	std::optional<d3d12UploadSpan_t> span = graph.constants.TryAllocate( size, PARM_ALIGN );
	while ( !span ) {
		graph.submitter.SubmitChunk();
		const uint64 oldest = graph.constants.OldestFence();
		if ( oldest == 0 ) {
			common->FatalError( "d3d12: %llu bytes of render graph parms do not fit into an empty ring", static_cast<unsigned long long>( bytes.size() ) );
		}
		graph.submitter.WaitFor( oldest );
		span = graph.constants.TryAllocate( size, PARM_ALIGN );
	}
	memcpy( span->cpu, bytes.data(), bytes.size() );
	return *span;
}

void idD3D12GraphContext::Dispatch( const d3d12ComputePipeline_t & pipeline, const d3d12UploadSpan_t * parms, uint32 threadsX, uint32 threadsY,
	uint32 threadsZ, std::span<const uint32> values ) {
	assert( pipeline && values.size() <= d3d12ComputeConstants_t::COUNT );
	ID3D12GraphicsCommandList4 & list = List();
	graph.barriers.Flush();
	if ( computeRootList != graph.submitter.ListSerial() ) {
		list.SetComputeRootSignature( graph.computeRoot.Get() );
		computeRootList = graph.submitter.ListSerial();
	}
	list.SetPipelineState( pipeline.pso.Get() );
	if ( parms != nullptr ) {
		list.SetComputeRootConstantBufferView( D3D12_ComputeRoot( d3d12ComputeSlot_t::PARMS ), parms->gpu );
	}
	if ( !values.empty() ) {
		list.SetComputeRoot32BitConstants( D3D12_ComputeRoot( d3d12ComputeSlot_t::CONSTANTS ), static_cast<UINT>( values.size() ), values.data(), 0 );
	}
	list.Dispatch( Groups( threadsX, pipeline.threadGroup[0] ), Groups( threadsY, pipeline.threadGroup[1] ), Groups( threadsZ, pipeline.threadGroup[2] ) );
}

void idD3D12GraphContext::Draw( ID3D12PipelineState & pso, d3d12GraphResource_t target, uint16 mip, const renderRect_t & viewport,
	const d3d12UploadSpan_t & parms ) {
	ID3D12GraphicsCommandList4 & list = List();
	graph.barriers.Flush();
	const D3D12_CPU_DESCRIPTOR_HANDLE rtv = Rtv( target, mip );
	list.OMSetRenderTargets( 1, &rtv, FALSE, nullptr );
	const D3D12_VIEWPORT view = { static_cast<float>( viewport.x ), static_cast<float>( viewport.y ),
		static_cast<float>( viewport.width ), static_cast<float>( viewport.height ), 0.0f, 1.0f };
	const D3D12_RECT scissor = { viewport.x, viewport.y, viewport.x + viewport.width, viewport.y + viewport.height };
	list.RSSetViewports( 1, &view );
	list.RSSetScissorRects( 1, &scissor );
	list.SetPipelineState( &pso );
	list.IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
	list.SetGraphicsRootConstantBufferView( D3D12_Root( d3d12RootSlot_t::FRAGMENT_PARMS ), parms.gpu );
	list.DrawInstanced( 3, 1, 0, 0 );
}

void idD3D12GraphContext::UavBarrier( d3d12GraphResource_t resource ) {
	CheckUse( resource, { d3d12GraphAccess_t::UAV } );
	graph.barriers.Uav( Resource( resource ) );
	graph.stats.barriers++;
}

// ---- the graph ---------------------------------------------------------------------------------------------

idD3D12RenderGraph::idD3D12RenderGraph( const idD3D12Device & owner, idD3D12Submitter & submit, idD3D12Barriers & batch, idD3D12ResourcePool & resourcePool,
	idD3D12DescriptorPool & srvs, idD3D12DescriptorPool & rtvs, idD3D12UploadRing & constantRing, const idD3D12ComputeRootSignature & root ) :
	device( owner ),
	submitter( submit ),
	barriers( batch ),
	pool( resourcePool ),
	srvPool( srvs ),
	rtvPool( rtvs ),
	constants( constantRing ),
	computeRoot( root ),
	context( *this ) {
}

d3d12GraphResource_t idD3D12RenderGraph::Create( const char * name, const d3d12PoolDesc_t & desc ) {
	assert( resources.size() < UINT16_MAX );
	resource_t & made = resources.emplace_back();
	made.name = name;
	made.desc = desc;
	return { static_cast<uint16>( resources.size() - 1 ) };
}

d3d12GraphResource_t idD3D12RenderGraph::Import( const char * name, const d3d12GraphImport_t & import ) {
	assert( resources.size() < UINT16_MAX && import.resource != nullptr && import.state != nullptr );
	resource_t & made = resources.emplace_back();
	made.name = name;
	made.import = import;
	made.imported = true;
	return { static_cast<uint16>( resources.size() - 1 ) };
}

void idD3D12RenderGraph::AddPass( const char * name, d3d12GraphPass_t type, std::span<const d3d12GraphUse_t> passUses,
	std::function<void( idD3D12GraphContext & )> record, bool neverCull ) {
	pass_t & pass = passes.emplace_back();
	pass.name = name;
	pass.type = type;
	pass.firstUse = static_cast<uint32>( uses.size() );
	pass.numUses = static_cast<uint32>( passUses.size() );
	pass.record = std::move( record );
	pass.neverCull = neverCull;
	for ( const d3d12GraphUse_t & use : passUses ) {
		assert( use.resource.index < resources.size() );
		assert( use.mip == d3d12GraphUse_t::ALL_MIPS || resources[use.resource.index].imported || use.mip < resources[use.resource.index].desc.mips );
		uses.push_back( use );
	}
}

// Backwards: a pass is kept when it writes an import or what a kept pass after it reads.
void idD3D12RenderGraph::Cull() {
	std::vector<bool> needed( resources.size(), false );
	for ( size_t p = passes.size(); p-- > 0; ) {
		pass_t & pass = passes[p];
		pass.kept = pass.neverCull || std::ranges::any_of( Uses( pass ), [this, &needed]( const d3d12GraphUse_t & use ) {
			return Writes( use.access ) && ( resources[use.resource.index].imported || needed[use.resource.index] );
		} );
		if ( !pass.kept ) {
			stats.culled++;
			continue;
		}
		stats.passes++;
		for ( const d3d12GraphUse_t & use : Uses( pass ) ) {
			if ( Reads( use.access ) ) {
				needed[use.resource.index] = true;
			}
		}
	}
}

// A UAV access after one as UAV waits for its writes; any transition out of UNORDERED_ACCESS does so anyway.
void idD3D12RenderGraph::Transition( const d3d12GraphUse_t & use ) {
	resource_t & made = resources[use.resource.index];
	if ( IsStructureAccess( use.access ) ) {
		assert( made.imported ? made.import.state->current == D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE
			: made.desc.kind == d3d12PoolDesc_t::kind_t::ACCELERATION_STRUCTURE );
		const bool hazard = ( made.lastStructureAccess == d3d12GraphAccess_t::AS_WRITE )
			|| ( use.access == d3d12GraphAccess_t::AS_WRITE && made.lastStructureAccess.has_value() );
		if ( hazard ) {
			barriers.Uav( made.imported ? *made.import.resource : *made.pooled->resource.Get() );
			stats.barriers++;
		}
		made.lastStructureAccess = use.access;
		return;
	}
	assert( use.access != d3d12GraphAccess_t::DEPTH_READ || made.imported );
	const D3D12_RESOURCE_STATES after = StateFor( use.access );
	const bool uav = ( use.access == d3d12GraphAccess_t::UAV );
	if ( made.imported ) {
		if ( uav && barriers.Current( *made.import.state ) == D3D12_RESOURCE_STATE_UNORDERED_ACCESS ) {
			barriers.Uav( *made.import.resource );
			stats.barriers++;
		}
		stats.barriers += ( barriers.Current( *made.import.state ) != after ) ? 1 : 0;
		barriers.Transition( *made.import.resource, *made.import.state, after );
		return;
	}
	d3d12PooledResource_t & pooled = *made.pooled;
	const int first = ( use.mip == d3d12GraphUse_t::ALL_MIPS ) ? 0 : use.mip;
	const int last = ( use.mip == d3d12GraphUse_t::ALL_MIPS ) ? static_cast<int>( pooled.states.size() ) - 1 : use.mip;
	bool waitForWrites = false;
	for ( int mip = first; mip <= last; mip++ ) {
		waitForWrites |= uav && barriers.Current( pooled.states[mip] ) == D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	}
	if ( waitForWrites ) {
		barriers.Uav( *pooled.resource.Get() );
		stats.barriers++;
	}
	for ( int mip = first; mip <= last; mip++ ) {
		stats.barriers += ( barriers.Current( pooled.states[mip] ) != after ) ? 1 : 0;
		barriers.Transition( *pooled.resource.Get(), pooled.states[mip], after, pooled.Subresource( mip ) );
	}
}

void idD3D12RenderGraph::Execute( idD3D12DebugGroups * groups ) {
	stats = {};
	if ( device.IsLost() ) {
		Clear();
		return;
	}
	Cull();
	for ( size_t p = 0; p < passes.size(); p++ ) {
		if ( !passes[p].kept ) {
			continue;
		}
		for ( const d3d12GraphUse_t & use : Uses( passes[p] ) ) {
			resource_t & made = resources[use.resource.index];
			if ( !made.imported ) {
				made.first = ( made.first < 0 ) ? static_cast<int>( p ) : made.first;
				made.last = static_cast<int>( p );
			}
		}
	}
	for ( const resource_t & made : resources ) {
		stats.transients += ( !made.imported && made.first >= 0 ) ? 1 : 0;
	}

	for ( size_t p = 0; p < passes.size(); p++ ) {
		pass_t & pass = passes[p];
		if ( !pass.kept ) {
			continue;
		}
		for ( resource_t & made : resources ) {
			if ( made.first == static_cast<int>( p ) && ( made.pooled = pool.Acquire( made.desc, made.name ) ) == nullptr ) {
				Clear();		// a lost device: nothing more is recorded
				return;
			}
		}
		for ( const d3d12GraphUse_t & use : Uses( pass ) ) {
			Transition( use );
		}
		if ( groups != nullptr ) {
			groups->Push( submitter.List(), pass.name );
		}
		barriers.Flush();
		context.pass = static_cast<int>( p );
		pass.record( context );
		context.pass = -1;
		if ( groups != nullptr ) {
			groups->Pop( &submitter.List() );
		}
		for ( resource_t & made : resources ) {
			if ( made.last == static_cast<int>( p ) ) {
				pool.Release( *made.pooled );
				made.pooled = nullptr;
			}
		}
	}
	lastStats = stats;
	Clear();
}

// Resources still held (an abandoned Execute) go back to the pool.
void idD3D12RenderGraph::Clear() {
	for ( resource_t & made : resources ) {
		if ( made.pooled != nullptr ) {
			pool.Release( *made.pooled );
		}
	}
	resources.clear();
	passes.clear();
	uses.clear();
}
