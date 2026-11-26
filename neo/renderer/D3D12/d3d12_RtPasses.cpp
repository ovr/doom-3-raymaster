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

// b0 of internal/rtReflect.hlsl.
struct rtReflectParms_t {
	float							clipToWorld[4][4];
	float							origin[4];		// w the radians per pixel
	float							viewport[4];
	float							limits[4];		// distance, the roughness that reflects nothing, the fade below it
	uint32							tlasSrv;
	uint32							recordsSrv;
	uint32							instancesSrv;
	uint32							verticesSrv;
	uint32							indexesSrv;
	uint32							skinnedSrv;
	uint32							lightsSrv;
	uint32							numLights;
	uint32							depthSrv;
	uint32							normalSrv;
	uint32							materialSrv;
	uint32							outputUav;
	uint32							linearSampler;
	uint32							clampSampler;
	uint32							maxLights;
	uint32							shadows;
};
static_assert( sizeof( rtReflectParms_t ) == 176 );

// b1 of internal/rtComposite.hlsl.
struct rtCompositeParms_t {
	int32							first[2];
	float							scale;
	uint32							reflectionSrv;
};

constexpr float REFLECTION_FADE = 0.15f;	// the roughness over which the reflections fade out below the limit

// What every trace reads, imported into one graph.
struct rtSceneImports_t {
	d3d12GraphResource_t			tlas;
	d3d12GraphResource_t			records;
	d3d12GraphResource_t			table;
	d3d12GraphResource_t			skinned;
};

[[nodiscard]] rtSceneImports_t ImportScene( idD3D12RenderGraph & graph, idD3D12RayTracing & rt, idD3D12GpuScene & gpuScene,
	std::vector<d3d12GraphUse_t> & uses ) {
	rtSceneImports_t imports;
	imports.tlas = graph.Import( "rt tlas", { rt.Tlas(), &rt.TlasState(), rt.TlasSrv(), {}, {}, 0, 0 } );
	imports.records = graph.Import( "gpu scene records", { gpuScene.Buffer(), &gpuScene.BufferState(), gpuScene.BufferSrv(), {}, {}, 0, 0 } );
	uses.push_back( { imports.tlas, d3d12GraphAccess_t::AS_READ } );
	uses.push_back( { imports.records, d3d12GraphAccess_t::SRV } );
	if ( rt.InstanceTable() != nullptr ) {
		imports.table = graph.Import( "rt instances", { rt.InstanceTable(), &rt.InstanceTableState(), rt.InstanceTableSrv(), {}, {}, 0, 0 } );
		uses.push_back( { imports.table, d3d12GraphAccess_t::SRV } );
	}
	if ( rt.SkinnedVertices() != nullptr ) {
		imports.skinned = graph.Import( "rt skinned vertices", { rt.SkinnedVertices(), &rt.SkinnedVerticesState(), rt.SkinnedVerticesSrv(), {}, {}, 0, 0 } );
		uses.push_back( { imports.skinned, d3d12GraphAccess_t::SRV } );
	}
	return imports;
}

// viewport inside the scene, empty when nothing of it is.
[[nodiscard]] renderRect_t SceneRect( const idD3D12SceneTarget & scene, const renderRect_t & viewport ) {
	const int x0 = Max( viewport.x, 0 );
	const int y0 = Max( viewport.y, 0 );
	const int x1 = Min( viewport.x + viewport.width, scene.Width() );
	const int y1 = Min( viewport.y + viewport.height, scene.Height() );
	return { x0, y0, Max( x1 - x0, 0 ), Max( y1 - y0, 0 ) };
}

}	// namespace

idD3D12RtPasses::idD3D12RtPasses( const idD3D12Device & owner, const idD3D12ComputeRootSignature & root, idD3D12InternalPasses & internalPasses,
	idD3D12SamplerHeap & samplerHeap, idD3D12BufferStore & bufferStore ) :
	device( owner ),
	computeRoot( root ),
	passes( internalPasses ),
	samplers( samplerHeap ),
	buffers( bufferStore ) {
}

void idD3D12RtPasses::CreateDefaults() {
	linearSampler = samplers.Acquire( D3D12_PackSampler( { samplerFilter_t::LINEAR_MIP_LINEAR, samplerAddress_t::REPEAT, 1.0f, 0.0f } ) );
	clampSampler = samplers.Acquire( D3D12_PackSampler( { samplerFilter_t::LINEAR_MIP_LINEAR, samplerAddress_t::CLAMP, 1.0f, 0.0f } ) );
}

ID3D12PipelineState * idD3D12RtPasses::DebugPipeline( UINT samples ) {
	idComPtr<ID3D12PipelineState> & pso = debug[std::countr_zero( samples ) & 3];
	if ( !pso ) {
		pso = passes.CreatePipeline( passes.Stage( ( samples > 1 ) ? "internal_rtDebug_msaa" : "internal_rtDebug" ).code,
			d3d12TargetId_t::Of( false, samples ).ColorView(), samples, static_cast<uint8>( D3D12_COLOR_WRITE_ENABLE_ALL ), L"doom3 ray tracing debug" );
	}
	return pso.Get();
}

ID3D12PipelineState * idD3D12RtPasses::CompositePipeline( UINT samples, bool add ) {
	idComPtr<ID3D12PipelineState> & pso = composite[( add ? 4 : 0 ) + ( std::countr_zero( samples ) & 3 )];
	if ( !pso ) {
		constexpr uint8 RGB = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
		pso = passes.CreatePipeline( passes.Stage( "internal_rtComposite" ).code, d3d12TargetId_t::HDR_FORMAT, samples, RGB,
			add ? L"doom3 rt reflection composite" : L"doom3 rt reflection alone", add ? d3d12PassBlend_t::ADD : d3d12PassBlend_t::REPLACE );
	}
	return pso.Get();
}

const d3d12ComputePipeline_t * idD3D12RtPasses::ReflectPipeline( bool multisampled ) {
	d3d12ComputePipeline_t & pipeline = reflect[multisampled ? 1 : 0];
	if ( !pipeline ) {
		const d3d12HlslStage_t & stage = passes.Stage( multisampled ? "internal_rtReflect_msaa" : "internal_rtReflect" );
		pipeline = D3D12_CreateComputePipeline( device, computeRoot, stage.code, stage.reflection, L"doom3 rt reflect" );
	}
	return pipeline ? &pipeline : nullptr;
}

bool idD3D12RtPasses::Traceable( idD3D12RayTracing & rt, idD3D12GpuScene & gpuScene ) {
	if ( !rt.Valid() || !gpuScene.Valid() || rt.Tlas() == nullptr || gpuScene.Buffer() == nullptr ) {
		return false;
	}
	// the draws since the build bound the static buffers as vertices and indexes again
	if ( buffers.UseForShaders( gpuScene.StaticVertexBuffer() ) == nullptr || buffers.UseForShaders( gpuScene.StaticIndexBuffer() ) == nullptr ) {
		return false;
	}
	gpuScene.MakeTexturesReadable();
	return true;
}

void idD3D12RtPasses::Debug( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene, idD3D12RayTracing & rt, idD3D12GpuScene & gpuScene,
	rtDebug_t mode, const rtView_t & view, const renderRect_t & viewport, idD3D12DebugGroups * groups ) {
	idD3D12SceneTarget::surface_t & output = scene.Color( false );
	idD3D12SceneTarget::surface_t & depth = scene.Depth();
	const renderRect_t rect = SceneRect( scene, viewport );
	if ( mode == rtDebug_t::NONE || !output.resource || !depth.resource || rect.width <= 0 || rect.height <= 0 || viewport.width <= 0 || viewport.height <= 0 ) {
		return;
	}
	ID3D12PipelineState * pso = DebugPipeline( scene.Samples() );
	if ( pso == nullptr || !Traceable( rt, gpuScene ) ) {
		return;
	}

	const uint32 width = static_cast<uint32>( scene.Width() );
	const uint32 height = static_cast<uint32>( scene.Height() );
	const d3d12GraphResource_t target = graph.Import( "Output", { output.resource.Get(), &output.state, output.srv, {}, scene.Rtv( false ), width, height } );
	const d3d12GraphResource_t depthUse = graph.Import( "Depth", { depth.resource.Get(), &depth.state, depth.srv, {}, {}, width, height } );
	std::vector<d3d12GraphUse_t> uses = { { target, d3d12GraphAccess_t::RTV, 0 }, { depthUse, d3d12GraphAccess_t::DEPTH_READ } };
	const rtSceneImports_t imports = ImportScene( graph, rt, gpuScene, uses );

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
	base.mode = static_cast<uint32>( mode );
	base.linearSampler = linearSampler;
	graph.AddPass( "rt debug", d3d12GraphPass_t::RASTER, uses, [pso, base, target, depthUse, imports, rect]( idD3D12GraphContext & context ) {
		rtDebugParms_t parms = base;
		parms.tlasSrv = context.Srv( imports.tlas );
		parms.recordsSrv = context.Srv( imports.records );
		parms.instancesSrv = imports.table.IsValid() ? context.Srv( imports.table ) : 0;
		parms.skinnedSrv = imports.skinned.IsValid() ? context.Srv( imports.skinned ) : 0;
		parms.depthSrv = context.Srv( depthUse );
		const d3d12UploadSpan_t span = context.Parms( parms );
		context.Draw( *pso, target, 0, rect, span );
	} );
	graph.Execute( groups );
}

void idD3D12RtPasses::Reflections( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene, idD3D12RayTracing & rt, idD3D12GpuScene & gpuScene,
	const rtReflectionSettings_t & settings, const rtView_t & view, const renderRect_t & viewport, float preExposure, idD3D12DebugGroups * groups ) {
	idD3D12SceneTarget::surface_t & sceneColor = scene.Color( true );
	idD3D12SceneTarget::surface_t & depth = scene.Depth();
	idD3D12SceneTarget::surface_t & normal = scene.GBuffer( 0 );
	idD3D12SceneTarget::surface_t & material = scene.GBuffer( 1 );
	const renderRect_t rect = SceneRect( scene, viewport );
	if ( !settings.enabled || !sceneColor.resource || !depth.resource || !normal.resource || !material.resource
			|| rect.width <= 0 || rect.height <= 0 || viewport.width <= 0 || viewport.height <= 0 ) {
		return;
	}
	const UINT samples = scene.Samples();
	const d3d12ComputePipeline_t * trace = ReflectPipeline( samples > 1 );
	ID3D12PipelineState * pso = CompositePipeline( samples, !settings.alone );
	if ( trace == nullptr || pso == nullptr || !Traceable( rt, gpuScene ) ) {
		return;
	}

	const uint32 width = static_cast<uint32>( scene.Width() );
	const uint32 height = static_cast<uint32>( scene.Height() );
	const d3d12GraphResource_t colorUse = graph.Import( "SceneColor", { sceneColor.resource.Get(), &sceneColor.state, sceneColor.srv, {},
		scene.Rtv( false, true ), width, height } );
	const d3d12GraphResource_t depthUse = graph.Import( "Depth", { depth.resource.Get(), &depth.state, depth.srv, {}, {}, width, height } );
	const d3d12GraphResource_t normalUse = graph.Import( "GBuffer normal", { normal.resource.Get(), &normal.state, normal.srv, {}, {}, width, height } );
	const d3d12GraphResource_t materialUse = graph.Import( "GBuffer material", { material.resource.Get(), &material.state, material.srv, {}, {},
		width, height } );
	d3d12PoolDesc_t desc;
	desc.format = d3d12TargetId_t::HDR_FORMAT;
	desc.width = static_cast<uint32>( viewport.width );
	desc.height = static_cast<uint32>( viewport.height );
	desc.unorderedAccess = true;
	const d3d12GraphResource_t reflection = graph.Create( "rt reflection", desc );
	std::vector<d3d12GraphUse_t> uses = { { depthUse, d3d12GraphAccess_t::DEPTH_READ }, { normalUse, d3d12GraphAccess_t::SRV },
		{ materialUse, d3d12GraphAccess_t::SRV }, { reflection, d3d12GraphAccess_t::UAV, 0 } };
	const rtSceneImports_t imports = ImportScene( graph, rt, gpuScene, uses );
	d3d12GraphResource_t lights;
	if ( gpuScene.NumLights() > 0 ) {
		lights = graph.Import( "gpu scene lights", { gpuScene.LightBuffer(), &gpuScene.LightBufferState(), gpuScene.LightBufferSrv(), {}, {}, 0, 0 } );
		uses.push_back( { lights, d3d12GraphAccess_t::SRV } );
	}

	rtReflectParms_t base = {};
	memcpy( base.clipToWorld, view.clipToWorld, sizeof( base.clipToWorld ) );
	base.origin[0] = view.origin[0];
	base.origin[1] = view.origin[1];
	base.origin[2] = view.origin[2];
	base.origin[3] = view.pixelSpread;
	base.viewport[0] = static_cast<float>( viewport.x );
	base.viewport[1] = static_cast<float>( viewport.y );
	base.viewport[2] = static_cast<float>( viewport.width );
	base.viewport[3] = static_cast<float>( viewport.height );
	base.limits[0] = Max( settings.distance, 1.0f );
	base.limits[1] = settings.maxRoughness;
	base.limits[2] = REFLECTION_FADE;
	base.verticesSrv = rt.StaticVerticesSrv().index;
	base.indexesSrv = rt.StaticIndexesSrv().index;
	base.numLights = static_cast<uint32>( gpuScene.NumLights() );
	base.linearSampler = linearSampler;
	base.clampSampler = clampSampler;
	base.maxLights = static_cast<uint32>( Max( settings.maxLights, 0 ) );
	base.shadows = settings.shadows ? 1u : 0u;
	const uint32 threadsX = static_cast<uint32>( viewport.width );
	const uint32 threadsY = static_cast<uint32>( viewport.height );
	graph.AddPass( "rt reflect", d3d12GraphPass_t::COMPUTE, uses, [trace, base, depthUse, normalUse, materialUse, reflection, imports, lights,
		threadsX, threadsY]( idD3D12GraphContext & context ) {
		rtReflectParms_t parms = base;
		parms.tlasSrv = context.Srv( imports.tlas );
		parms.recordsSrv = context.Srv( imports.records );
		parms.instancesSrv = imports.table.IsValid() ? context.Srv( imports.table ) : 0;
		parms.skinnedSrv = imports.skinned.IsValid() ? context.Srv( imports.skinned ) : 0;
		parms.lightsSrv = lights.IsValid() ? context.Srv( lights ) : 0;
		parms.depthSrv = context.Srv( depthUse );
		parms.normalSrv = context.Srv( normalUse );
		parms.materialSrv = context.Srv( materialUse );
		parms.outputUav = context.Uav( reflection );
		const d3d12UploadSpan_t span = context.Parms( parms );
		context.Dispatch( *trace, &span, threadsX, threadsY );
	} );

	const rtCompositeParms_t compositeBase = { { viewport.x, viewport.y }, settings.scale * preExposure, 0 };
	graph.AddPass( "rt composite", d3d12GraphPass_t::RASTER, { { reflection, d3d12GraphAccess_t::SRV }, { colorUse, d3d12GraphAccess_t::RTV, 0 } },
		[pso, compositeBase, reflection, colorUse, rect]( idD3D12GraphContext & context ) {
			rtCompositeParms_t parms = compositeBase;
			parms.reflectionSrv = context.Srv( reflection );
			const d3d12UploadSpan_t span = context.Parms( parms );
			context.Draw( *pso, colorUse, 0, rect, span );
		} );
	graph.Execute( groups );
}
