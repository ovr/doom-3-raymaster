// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#include "../../idlib/precompiled.h"

#include <bit>

#include "../tr_local.h"
#include "d3d12_InternalPasses.h"
#include "d3d12_Output.h"
#include "d3d12_RtPasses.h"

namespace {

// b1 of internal/rtDebug.hlsl.
struct rtDebugParms_t {
	float							clipToWorld[4][4];
	float							origin[4];		// w the radians per pixel
	float							viewport[4];
	uint32							tlasSrv;
	uint32							recordsSrv;
	uint32							instancesSrv;
	uint32							verticesSrv;
	uint32							indexesSrv;
	uint32							skinnedSrv;
	uint32							depthSrv;
	uint32							mode;
	uint32							linearSampler;
	uint32							unused[3];
};
static_assert( sizeof( rtDebugParms_t ) == 144 );

}	// namespace

idD3D12RtPasses::idD3D12RtPasses( idD3D12InternalPasses & internalPasses, idD3D12SamplerHeap & samplerHeap, idD3D12BufferStore & bufferStore ) :
	passes( internalPasses ),
	samplers( samplerHeap ),
	buffers( bufferStore ) {
}

void idD3D12RtPasses::CreateDefaults() {
	linearSampler = samplers.Acquire( D3D12_PackSampler( { samplerFilter_t::LINEAR_MIP_LINEAR, samplerAddress_t::REPEAT, 1.0f, 0.0f } ) );
}

ID3D12PipelineState * idD3D12RtPasses::DebugPipeline( UINT samples ) {
	idComPtr<ID3D12PipelineState> & pso = debug[std::countr_zero( samples ) & 3];
	if ( !pso ) {
		pso = passes.CreatePipeline( passes.Stage( ( samples > 1 ) ? "internal_rtDebug_msaa" : "internal_rtDebug" ).code,
			d3d12TargetId_t::Of( false, samples ).ColorView(), samples, static_cast<uint8>( D3D12_COLOR_WRITE_ENABLE_ALL ), L"doom3 ray tracing debug" );
	}
	return pso.Get();
}

void idD3D12RtPasses::Debug( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene, idD3D12RayTracing & rt, idD3D12GpuScene & gpuScene,
	const rtDebugView_t & view, const renderRect_t & viewport, idD3D12DebugGroups * groups ) {
	if ( view.mode == rtDebug_t::NONE || !rt.Valid() || !gpuScene.Valid() || rt.Tlas() == nullptr || gpuScene.Buffer() == nullptr ) {
		return;
	}
	idD3D12SceneTarget::surface_t & output = scene.Color( false );
	idD3D12SceneTarget::surface_t & depth = scene.Depth();
	ID3D12PipelineState * pso = DebugPipeline( scene.Samples() );
	const int x0 = Max( viewport.x, 0 );
	const int y0 = Max( viewport.y, 0 );
	const renderRect_t rect = { x0, y0, Min( viewport.x + viewport.width, scene.Width() ) - x0, Min( viewport.y + viewport.height, scene.Height() ) - y0 };
	if ( !output.resource || !depth.resource || pso == nullptr || rect.width <= 0 || rect.height <= 0 || viewport.width <= 0 || viewport.height <= 0 ) {
		return;
	}
	// the draws since the build bound the static buffers as vertices and indexes again
	if ( buffers.UseForShaders( gpuScene.StaticVertexBuffer() ) == nullptr || buffers.UseForShaders( gpuScene.StaticIndexBuffer() ) == nullptr ) {
		return;
	}
	gpuScene.MakeTexturesReadable();

	const uint32 width = static_cast<uint32>( scene.Width() );
	const uint32 height = static_cast<uint32>( scene.Height() );
	const d3d12GraphResource_t target = graph.Import( "Output", { output.resource.Get(), &output.state, output.srv, {}, scene.Rtv( false ), width, height } );
	const d3d12GraphResource_t depthUse = graph.Import( "Depth", { depth.resource.Get(), &depth.state, depth.srv, {}, {}, width, height } );
	const d3d12GraphResource_t tlas = graph.Import( "rt tlas", { rt.Tlas(), &rt.TlasState(), rt.TlasSrv(), {}, {}, 0, 0 } );
	const d3d12GraphResource_t records = graph.Import( "gpu scene records", { gpuScene.Buffer(), &gpuScene.BufferState(), gpuScene.BufferSrv(), {}, {}, 0, 0 } );
	std::vector<d3d12GraphUse_t> uses = { { target, d3d12GraphAccess_t::RTV, 0 }, { depthUse, d3d12GraphAccess_t::DEPTH_READ },
		{ tlas, d3d12GraphAccess_t::AS_READ }, { records, d3d12GraphAccess_t::SRV } };
	d3d12GraphResource_t table;
	if ( rt.InstanceTable() != nullptr ) {
		table = graph.Import( "rt instances", { rt.InstanceTable(), &rt.InstanceTableState(), rt.InstanceTableSrv(), {}, {}, 0, 0 } );
		uses.push_back( { table, d3d12GraphAccess_t::SRV } );
	}
	d3d12GraphResource_t skinned;
	if ( rt.SkinnedVertices() != nullptr ) {
		skinned = graph.Import( "rt skinned vertices", { rt.SkinnedVertices(), &rt.SkinnedVerticesState(), rt.SkinnedVerticesSrv(), {}, {}, 0, 0 } );
		uses.push_back( { skinned, d3d12GraphAccess_t::SRV } );
	}

	rtDebugParms_t base = {};
	memcpy( base.clipToWorld, view.clipToWorld, sizeof( base.clipToWorld ) );
	base.origin[0] = view.origin[0];
	base.origin[1] = view.origin[1];
	base.origin[2] = view.origin[2];
	base.origin[3] = view.pixelSpread;
	base.viewport[0] = static_cast<float>( viewport.x );
	base.viewport[1] = static_cast<float>( viewport.y );
	base.viewport[2] = static_cast<float>( viewport.width );
	base.viewport[3] = static_cast<float>( viewport.height );
	base.verticesSrv = rt.StaticVerticesSrv().index;
	base.indexesSrv = rt.StaticIndexesSrv().index;
	base.mode = static_cast<uint32>( view.mode );
	base.linearSampler = linearSampler;
	graph.AddPass( "rt debug", d3d12GraphPass_t::RASTER, uses, [pso, base, target, depthUse, tlas, records, table, skinned, rect](
		idD3D12GraphContext & context ) {
		rtDebugParms_t parms = base;
		parms.tlasSrv = context.Srv( tlas );
		parms.recordsSrv = context.Srv( records );
		parms.instancesSrv = table.IsValid() ? context.Srv( table ) : 0;
		parms.skinnedSrv = skinned.IsValid() ? context.Srv( skinned ) : 0;
		parms.depthSrv = context.Srv( depthUse );
		const d3d12UploadSpan_t span = context.Parms( parms );
		context.Draw( *pso, target, 0, rect, span );
	} );
	graph.Execute( groups );
}
