// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#include "../../idlib/precompiled.h"

#include <bit>

#include "../tr_local.h"
#include "../BlueNoise.h"
#include "d3d12_InternalPasses.h"
#include "d3d12_Output.h"
#include "d3d12_RtHistory.h"
#include "d3d12_RtPasses.h"
#include "d3d12_Textures.h"

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

// b1 of internal/rtShadowRatio.hlsl.
struct rtShadowRatioParms_t {
	int32							first[2];
	uint32							sceneSrv;
	uint32							lightsSrv;
	uint32							samples;
};

// b1 of internal/rtShadowApply.hlsl.
struct rtShadowApplyParms_t {
	int32							first[2];
	uint32							ratioSrv;
	float							show;		// 0, or the scale of the picture that takes the scene's place
	uint32							reachSrv;
	uint32							lengthSrv;
	float							lengthScale;	// 0, or 1 over the frames a history may hold: the picture is its length
	uint32							unused;
};
static_assert( sizeof( rtShadowApplyParms_t ) == 32 );

// b0 of internal/rtGuides.hlsl.
struct rtGuidesParms_t {
	float							clipToWorld[4][4];
	float							prevClipW[4];
	float							viewport[4];
	int32							first[2];
	uint32							size[2];
	uint32							depthSrv;
	uint32							normalSrv;
	uint32							motionSrv;
	uint32							albedoSrv;
	uint32							prevGuidesSrv;
	uint32							guidesUav;
	uint32							reprojectUav;
	uint32							slopeUav;
	uint32							flags;		// bit 0: prevGuidesSrv is there
	uint32							unused[3];
};
static_assert( sizeof( rtGuidesParms_t ) == 160 );

// b0 of internal/rtTemporal.hlsl.
struct rtTemporalParms_t {
	uint32							size[2];
	uint32							signalSrv;
	uint32							guidesSrv;
	uint32							reprojectSrv;
	uint32							slopeSrv;
	uint32							prevGuidesSrv;
	uint32							prevColorSrv;
	uint32							prevAuxSrv;
	uint32							colorUav;
	uint32							auxUav;
	uint32							outputUav;
	uint32							flags;		// bit 0: the three of the frame before are there; bit 1: a plain average, nothing cut
	float							maxHistory;
	float							invPreExposure;
	float							leastStep;	// the last bit of what the average is kept in, over its value; 0: no least step
};
static_assert( sizeof( rtTemporalParms_t ) == 64 );

// b0 of internal/rtAtrous.hlsl.
struct rtAtrousParms_t {
	uint32							size[2];
	uint32							inputSrv;
	uint32							guidesSrv;
	uint32							slopeSrv;
	uint32							outputUav;
	int32							stride;
	uint32							unused;
};
static_assert( sizeof( rtAtrousParms_t ) == 32 );

constexpr int MAX_ATROUS_PASSES = 5;
constexpr DXGI_FORMAT GUIDES_FORMAT = DXGI_FORMAT_R32_UINT;
constexpr DXGI_FORMAT MOMENTS_FORMAT = DXGI_FORMAT_R16G16_FLOAT;

[[nodiscard]] d3d12GraphResource_t ImportHistory( idD3D12RenderGraph & graph, const char * name, idD3D12RtHistory::surface_t & surface, uint32 width,
	uint32 height ) {
	return graph.Import( name, { surface.resource.Get(), &surface.state, surface.srv, surface.uav, {}, width, height } );
}

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
	idD3D12SamplerHeap & samplerHeap, idD3D12BufferStore & bufferStore, idD3D12TextureStore & textureStore, idD3D12Barriers & batch ) :
	device( owner ),
	computeRoot( root ),
	passes( internalPasses ),
	samplers( samplerHeap ),
	buffers( bufferStore ),
	textures( textureStore ),
	barriers( batch ) {
}

// The noise is two bytes a texel, which FMT_L8A8 shows as ( x, x, x, w ): rtNoise.hlsli loads .xw.
void idD3D12RtPasses::CreateDefaults() {
	linearSampler = samplers.Acquire( D3D12_PackSampler( { samplerFilter_t::LINEAR_MIP_LINEAR, samplerAddress_t::REPEAT, 1.0f, 0.0f } ) );
	clampSampler = samplers.Acquire( D3D12_PackSampler( { samplerFilter_t::LINEAR_MIP_LINEAR, samplerAddress_t::CLAMP, 1.0f, 0.0f } ) );
	idImageOpts opts;
	opts.textureType = TT_2D;
	opts.format = FMT_L8A8;
	opts.colorFormat = CFM_DEFAULT;
	opts.width = BLUE_NOISE_SIZE;
	opts.height = BLUE_NOISE_SIZE;
	opts.numLevels = 1;
	noise = textures.Create( opts, textureRole_t::SAMPLED, { samplerFilter_t::NEAREST, samplerAddress_t::REPEAT, 1.0f, 0.0f }, "_rtBlueNoise" );
	textures.Upload( noise, 0, 0, BLUE_NOISE_SIZE, BLUE_NOISE_SIZE, blueNoise, BLUE_NOISE_SIZE );
}

void idD3D12RtPasses::DestroyDefaults() {
	textures.Destroy( noise );
	noise = textureHandle_t::NONE;
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

ID3D12PipelineState * idD3D12RtPasses::ShadowRatioPipeline( bool multisampled ) {
	idComPtr<ID3D12PipelineState> & pso = shadowRatio[multisampled ? 1 : 0];
	if ( !pso ) {
		pso = passes.CreatePipeline( passes.Stage( multisampled ? "internal_rtShadowRatio_msaa" : "internal_rtShadowRatio" ).code,
			d3d12TargetId_t::HDR_FORMAT, 1, static_cast<uint8>( D3D12_COLOR_WRITE_ENABLE_ALL ), L"doom3 rt shadow ratio" );
	}
	return pso.Get();
}

ID3D12PipelineState * idD3D12RtPasses::ShadowApplyPipeline( UINT samples, bool show ) {
	idComPtr<ID3D12PipelineState> & pso = shadowApply[( show ? 4 : 0 ) + ( std::countr_zero( samples ) & 3 )];
	if ( !pso ) {
		constexpr uint8 RGB = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
		pso = passes.CreatePipeline( passes.Stage( "internal_rtShadowApply" ).code, d3d12TargetId_t::HDR_FORMAT, samples, RGB,
			show ? L"doom3 rt shadows alone" : L"doom3 rt shadow apply", show ? d3d12PassBlend_t::REPLACE : d3d12PassBlend_t::MULTIPLY );
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

const d3d12ComputePipeline_t * idD3D12RtPasses::ComputePipeline( d3d12ComputePipeline_t & pipeline, const char * stageName, const wchar_t * name ) {
	if ( !pipeline ) {
		const d3d12HlslStage_t & stage = passes.Stage( stageName );
		pipeline = D3D12_CreateComputePipeline( device, computeRoot, stage.code, stage.reflection, name );
	}
	return pipeline ? &pipeline : nullptr;
}

bool idD3D12RtPasses::Traceable( idD3D12RayTracing & rt, idD3D12GpuScene & gpuScene ) {
	if ( !rt.Valid() || !gpuScene.Valid() || rt.Tlas() == nullptr || gpuScene.Buffer() == nullptr ) {
		return false;
	}
	// they decay with every list
	if ( buffers.Use( gpuScene.StaticVertexBuffer() ) == nullptr || buffers.Use( gpuScene.StaticIndexBuffer() ) == nullptr ) {
		return false;
	}
	// a texture leaves its read state only through the store, which counts it: no walk per draw
	const std::array<uint64, 3> now = { gpuScene.SceneId(), gpuScene.Sequence(), textures.StateChanges() };
	if ( readable != now ) {
		gpuScene.MakeTexturesReadable();
		readable = now;
	}
	return true;
}

d3d12TraceResources_t idD3D12RtPasses::DrawResources( idD3D12RayTracing & rt, idD3D12GpuScene & gpuScene ) {
	d3d12TraceResources_t trace;
	if ( !Traceable( rt, gpuScene ) ) {
		return trace;
	}
	// what the graph's passes import, here for a draw of the open list; the TLAS has one state for life
	barriers.Transition( *gpuScene.Buffer(), gpuScene.BufferState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE );
	if ( rt.InstanceTable() != nullptr ) {
		barriers.Transition( *rt.InstanceTable(), rt.InstanceTableState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE );
		trace.indices[d3d12TraceResources_t::INSTANCES] = rt.InstanceTableSrv().index;
	}
	if ( rt.SkinnedVertices() != nullptr ) {
		barriers.Transition( *rt.SkinnedVertices(), rt.SkinnedVerticesState(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE );
		trace.indices[d3d12TraceResources_t::SKINNED] = rt.SkinnedVerticesSrv().index;
	}
	textures.MakeReadable( noise );
	trace.indices[d3d12TraceResources_t::TLAS] = rt.TlasSrv().index;
	trace.indices[d3d12TraceResources_t::RECORDS] = gpuScene.BufferSrv().index;
	trace.indices[d3d12TraceResources_t::VERTICES] = rt.StaticVerticesSrv().index;
	trace.indices[d3d12TraceResources_t::INDEXES] = rt.StaticIndexesSrv().index;
	trace.indices[d3d12TraceResources_t::NOISE] = textures.SampledView( noise, textureKind_t::TEX_2D ).index;
	trace.indices[d3d12TraceResources_t::SAMPLER] = linearSampler;
	trace.indices[d3d12TraceResources_t::FLAGS] = d3d12TraceResources_t::VALID;
	return trace;
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

// The guides are a texel per pixel of rect, also under MSAA (sample 0). With a history and view.store they are the
// eye's for the next frame; a screenshot's and a filter's without history live for the graph only.
std::optional<idD3D12RtPasses::guideUses_t> idD3D12RtPasses::AddGuides( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene,
	idD3D12RtHistory * history, const rtView_t & view, const renderRect_t & viewport, const renderRect_t & rect ) {
	idD3D12SceneTarget::surface_t & depth = scene.Depth();
	idD3D12SceneTarget::surface_t & normal = scene.GBuffer( 0 );
	idD3D12SceneTarget::surface_t & motion = scene.GBuffer( 2 );
	idD3D12SceneTarget::surface_t & albedo = scene.GBuffer( 3 );
	if ( !depth.resource || !normal.resource || !motion.resource || !albedo.resource ) {
		return std::nullopt;
	}
	const bool multisampled = scene.Samples() > 1;
	const d3d12ComputePipeline_t * pipeline = ComputePipeline( guides[multisampled ? 1 : 0], multisampled ? "internal_rtGuides_msaa" : "internal_rtGuides",
		L"doom3 rt guides" );
	if ( pipeline == nullptr ) {
		return std::nullopt;
	}

	const uint32 sceneWidth = static_cast<uint32>( scene.Width() );
	const uint32 sceneHeight = static_cast<uint32>( scene.Height() );
	const uint32 width = static_cast<uint32>( rect.width );
	const uint32 height = static_cast<uint32>( rect.height );
	d3d12PoolDesc_t desc;
	desc.format = GUIDES_FORMAT;
	desc.width = width;
	desc.height = height;
	desc.unorderedAccess = true;
	guideUses_t made;
	idD3D12RtHistory::use_t held;
	if ( history != nullptr ) {
		held = history->Use( rtHistorySlot_t::GUIDES, GUIDES_FORMAT, width, height, view );
	}
	if ( held.next != nullptr && view.store ) {
		made.guides = ImportHistory( graph, "rt guides", *held.next, width, height );
		history->Written( rtHistorySlot_t::GUIDES, view );
	} else {
		made.guides = graph.Create( "rt guides", desc );
	}
	if ( held.previous != nullptr ) {
		made.previous = ImportHistory( graph, "rt guides before", *held.previous, width, height );
	}
	desc.format = d3d12TargetId_t::HDR_FORMAT;
	made.reproject = graph.Create( "rt reproject", desc );
	desc.format = MOMENTS_FORMAT;
	made.slope = graph.Create( "rt slope", desc );

	const d3d12GraphResource_t depthUse = graph.Import( "Depth", { depth.resource.Get(), &depth.state, depth.srv, {}, {}, sceneWidth, sceneHeight } );
	const d3d12GraphResource_t normalUse = graph.Import( "GBuffer normal", { normal.resource.Get(), &normal.state, normal.srv, {}, {},
		sceneWidth, sceneHeight } );
	const d3d12GraphResource_t motionUse = graph.Import( "GBuffer motion", { motion.resource.Get(), &motion.state, motion.srv, {}, {},
		sceneWidth, sceneHeight } );
	const d3d12GraphResource_t albedoUse = graph.Import( "GBuffer albedo", { albedo.resource.Get(), &albedo.state, albedo.srv, {}, {},
		sceneWidth, sceneHeight } );
	std::vector<d3d12GraphUse_t> uses = { { depthUse, d3d12GraphAccess_t::DEPTH_READ }, { normalUse, d3d12GraphAccess_t::SRV },
		{ motionUse, d3d12GraphAccess_t::SRV }, { albedoUse, d3d12GraphAccess_t::SRV }, { made.guides, d3d12GraphAccess_t::UAV, 0 },
		{ made.reproject, d3d12GraphAccess_t::UAV, 0 }, { made.slope, d3d12GraphAccess_t::UAV, 0 } };
	if ( made.previous.IsValid() ) {
		uses.push_back( { made.previous, d3d12GraphAccess_t::SRV } );
	}

	rtGuidesParms_t base = {};
	memcpy( base.clipToWorld, view.clipToWorld, sizeof( base.clipToWorld ) );
	memcpy( base.prevClipW, view.prevWorldToClip[3], sizeof( base.prevClipW ) );
	base.viewport[0] = static_cast<float>( viewport.x );
	base.viewport[1] = static_cast<float>( viewport.y );
	base.viewport[2] = static_cast<float>( viewport.width );
	base.viewport[3] = static_cast<float>( viewport.height );
	base.first[0] = rect.x;
	base.first[1] = rect.y;
	base.size[0] = width;
	base.size[1] = height;
	base.flags = made.previous.IsValid() ? 1u : 0u;
	graph.AddPass( "rt guides", d3d12GraphPass_t::COMPUTE, uses, [pipeline, base, depthUse, normalUse, motionUse, albedoUse, made]( idD3D12GraphContext & context ) {
		rtGuidesParms_t parms = base;
		parms.depthSrv = context.Srv( depthUse );
		parms.normalSrv = context.Srv( normalUse );
		parms.motionSrv = context.Srv( motionUse );
		parms.albedoSrv = context.Srv( albedoUse );
		parms.prevGuidesSrv = made.previous.IsValid() ? context.Srv( made.previous ) : 0;
		parms.guidesUav = context.Uav( made.guides );
		parms.reprojectUav = context.Uav( made.reproject );
		parms.slopeUav = context.Uav( made.slope );
		const d3d12UploadSpan_t span = context.Parms( parms );
		context.Dispatch( *pipeline, &span, parms.size[0], parms.size[1] );
	} );
	return made;
}

// The average and its moments are read from the frame before and written for the next one only with a history;
// without one (r_rtHistory 0, or a screenshot's writes) they are transients, and every pixel starts at one frame.
d3d12GraphResource_t idD3D12RtPasses::FilterShadows( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene, idD3D12RtHistory & history,
	const rtShadowSettings_t & settings, const rtView_t & view, const renderRect_t & viewport, const renderRect_t & rect, d3d12GraphResource_t ratio,
	float preExposure, d3d12GraphResource_t & length ) {
	const int numPasses = Min( Max( settings.passes, 0 ), MAX_ATROUS_PASSES );
	const d3d12ComputePipeline_t * temporalPipeline = ComputePipeline( temporal, "internal_rtTemporal", L"doom3 rt temporal" );
	const d3d12ComputePipeline_t * atrousPipeline = ( numPasses > 0 ) ? ComputePipeline( atrous, "internal_rtAtrous", L"doom3 rt atrous" ) : nullptr;
	if ( temporalPipeline == nullptr || ( numPasses > 0 && atrousPipeline == nullptr ) ) {
		return ratio;
	}
	const std::optional<guideUses_t> guide = AddGuides( graph, scene, settings.history ? &history : nullptr, view, viewport, rect );
	if ( !guide ) {
		return ratio;
	}

	const uint32 width = static_cast<uint32>( rect.width );
	const uint32 height = static_cast<uint32>( rect.height );
	// a half float stops counting frames at 2048
	const DXGI_FORMAT colorFormat = settings.reference ? DXGI_FORMAT_R32G32B32A32_FLOAT : d3d12TargetId_t::HDR_FORMAT;
	idD3D12RtHistory::use_t heldColor, heldMoments;
	if ( settings.history ) {
		heldColor = history.Use( rtHistorySlot_t::SHADOW_COLOR, colorFormat, width, height, view );
		heldMoments = history.Use( rtHistorySlot_t::SHADOW_AUX, MOMENTS_FORMAT, width, height, view );
	}
	const bool store = view.store && heldColor.next != nullptr && heldMoments.next != nullptr;
	const bool read = guide->previous.IsValid() && heldColor.previous != nullptr && heldMoments.previous != nullptr;
	d3d12PoolDesc_t desc;
	desc.width = width;
	desc.height = height;
	desc.unorderedAccess = true;
	d3d12GraphResource_t color, moments, prevColor, prevMoments;
	if ( store ) {
		color = ImportHistory( graph, "rt shadow history", *heldColor.next, width, height );
		moments = ImportHistory( graph, "rt shadow moments", *heldMoments.next, width, height );
		history.Written( rtHistorySlot_t::SHADOW_COLOR, view );
		history.Written( rtHistorySlot_t::SHADOW_AUX, view );
	} else {
		desc.format = colorFormat;
		color = graph.Create( "rt shadow history", desc );
		desc.format = MOMENTS_FORMAT;
		moments = graph.Create( "rt shadow moments", desc );
	}
	desc.format = d3d12TargetId_t::HDR_FORMAT;
	d3d12GraphResource_t filtered = graph.Create( "rt shadow mean", desc );
	std::vector<d3d12GraphUse_t> uses = { { ratio, d3d12GraphAccess_t::SRV }, { guide->guides, d3d12GraphAccess_t::SRV },
		{ guide->reproject, d3d12GraphAccess_t::SRV }, { guide->slope, d3d12GraphAccess_t::SRV }, { color, d3d12GraphAccess_t::UAV, 0 },
		{ moments, d3d12GraphAccess_t::UAV, 0 }, { filtered, d3d12GraphAccess_t::UAV, 0 } };
	if ( read ) {
		prevColor = ImportHistory( graph, "rt shadow history before", *heldColor.previous, width, height );
		prevMoments = ImportHistory( graph, "rt shadow moments before", *heldMoments.previous, width, height );
		uses.push_back( { guide->previous, d3d12GraphAccess_t::SRV } );
		uses.push_back( { prevColor, d3d12GraphAccess_t::SRV } );
		uses.push_back( { prevMoments, d3d12GraphAccess_t::SRV } );
	}

	rtTemporalParms_t temporalBase = {};
	temporalBase.size[0] = width;
	temporalBase.size[1] = height;
	temporalBase.flags = ( read ? 1u : 0u ) | ( settings.reference ? 2u : 0u );
	temporalBase.maxHistory = static_cast<float>( Max( settings.maxHistory, 1 ) );
	temporalBase.invPreExposure = 1.0f / Max( preExposure, 1e-6f );
	// a half float's; the reference counts every frame alike in 32 bits
	temporalBase.leastStep = settings.reference ? 0.0f : 1.0f / 1024.0f;
	const guideUses_t frame = *guide;
	graph.AddPass( "rt shadow temporal", d3d12GraphPass_t::COMPUTE, uses, [temporalPipeline, temporalBase, ratio, frame, color, moments, prevColor,
		prevMoments, filtered, read]( idD3D12GraphContext & context ) {
		rtTemporalParms_t parms = temporalBase;
		parms.signalSrv = context.Srv( ratio );
		parms.guidesSrv = context.Srv( frame.guides );
		parms.reprojectSrv = context.Srv( frame.reproject );
		parms.slopeSrv = context.Srv( frame.slope );
		parms.prevGuidesSrv = read ? context.Srv( frame.previous ) : 0;
		parms.prevColorSrv = read ? context.Srv( prevColor ) : 0;
		parms.prevAuxSrv = read ? context.Srv( prevMoments ) : 0;
		parms.colorUav = context.Uav( color );
		parms.auxUav = context.Uav( moments );
		parms.outputUav = context.Uav( filtered );
		const d3d12UploadSpan_t span = context.Parms( parms );
		context.Dispatch( *temporalPipeline, &span, parms.size[0], parms.size[1] );
	} );

	// the graph keeps the names
	static constexpr std::array<const char *, MAX_ATROUS_PASSES> NAMES = { "rt shadow atrous 1", "rt shadow atrous 2", "rt shadow atrous 3",
		"rt shadow atrous 4", "rt shadow atrous 5" };
	for ( int pass = 0; pass < numPasses; pass++ ) {
		const d3d12GraphResource_t wider = graph.Create( NAMES[pass], desc );
		const rtAtrousParms_t atrousBase = { { width, height }, 0, 0, 0, 0, 1 << pass, 0 };
		graph.AddPass( NAMES[pass], d3d12GraphPass_t::COMPUTE, { { filtered, d3d12GraphAccess_t::SRV }, { frame.guides, d3d12GraphAccess_t::SRV },
			{ frame.slope, d3d12GraphAccess_t::SRV }, { wider, d3d12GraphAccess_t::UAV, 0 } },
			[atrousPipeline, atrousBase, filtered, frame, wider]( idD3D12GraphContext & context ) {
				rtAtrousParms_t parms = atrousBase;
				parms.inputSrv = context.Srv( filtered );
				parms.guidesSrv = context.Srv( frame.guides );
				parms.slopeSrv = context.Srv( frame.slope );
				parms.outputUav = context.Uav( wider );
				const d3d12UploadSpan_t span = context.Parms( parms );
				context.Dispatch( *atrousPipeline, &span, parms.size[0], parms.size[1] );
			} );
		filtered = wider;
	}
	if ( settings.history ) {
		length = color;
	}
	return filtered;
}

// The ratio is one value per pixel, also under MSAA: the pixel's samples lose the same part, and their sum what the
// light target's sum holds. It is a signal of its own so that a filter can work on it and leave the scene's detail.
void idD3D12RtPasses::Shadows( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene, idD3D12RtHistory & history,
	const rtShadowSettings_t & settings, const rtView_t & view, const renderRect_t & viewport, float preExposure, idD3D12DebugGroups * groups ) {
	idD3D12SceneTarget::surface_t & sceneColor = scene.Color( true );
	idD3D12SceneTarget::surface_t & lights = scene.Lights();
	const renderRect_t rect = SceneRect( scene, viewport );
	if ( !settings.enabled || !sceneColor.resource || !lights.resource || rect.width <= 0 || rect.height <= 0 ) {
		return;
	}
	const UINT samples = scene.Samples();
	const bool picture = settings.show || settings.showLength;
	ID3D12PipelineState * ratioPso = ShadowRatioPipeline( samples > 1 );
	ID3D12PipelineState * applyPso = ShadowApplyPipeline( samples, picture );
	if ( ratioPso == nullptr || applyPso == nullptr ) {
		return;
	}

	const uint32 width = static_cast<uint32>( scene.Width() );
	const uint32 height = static_cast<uint32>( scene.Height() );
	const d3d12GraphResource_t colorUse = graph.Import( "SceneColor", { sceneColor.resource.Get(), &sceneColor.state, sceneColor.srv, {},
		scene.Rtv( false, true ), width, height } );
	const d3d12GraphResource_t lightsUse = graph.Import( "Lights", { lights.resource.Get(), &lights.state, lights.srv, {}, {}, width, height } );
	d3d12PoolDesc_t desc;
	desc.format = d3d12TargetId_t::HDR_FORMAT;
	desc.width = static_cast<uint32>( rect.width );
	desc.height = static_cast<uint32>( rect.height );
	desc.renderTarget = true;
	const d3d12GraphResource_t ratio = graph.Create( "rt shadow ratio", desc );

	const rtShadowRatioParms_t ratioBase = { { rect.x, rect.y }, 0, 0, samples };
	const renderRect_t ratioRect = { 0, 0, rect.width, rect.height };
	graph.AddPass( "rt shadow ratio", d3d12GraphPass_t::RASTER, { { colorUse, d3d12GraphAccess_t::SRV }, { lightsUse, d3d12GraphAccess_t::SRV },
		{ ratio, d3d12GraphAccess_t::RTV, 0 } }, [ratioPso, ratioBase, colorUse, lightsUse, ratio, ratioRect]( idD3D12GraphContext & context ) {
			rtShadowRatioParms_t parms = ratioBase;
			parms.sceneSrv = context.Srv( colorUse );
			parms.lightsSrv = context.Srv( lightsUse );
			const d3d12UploadSpan_t span = context.Parms( parms );
			context.Draw( *ratioPso, ratio, 0, ratioRect, span );
		} );

	// R18: what is left, over the frames and the neighbours of the same surface
	d3d12GraphResource_t length;
	const d3d12GraphResource_t left = settings.Filtered()
		? FilterShadows( graph, scene, history, settings, view, viewport, rect, ratio, preExposure, length ) : ratio;
	const bool showLength = settings.showLength && length.IsValid();

	rtShadowApplyParms_t applyBase = {};
	applyBase.first[0] = rect.x;
	applyBase.first[1] = rect.y;
	applyBase.show = picture ? preExposure : 0.0f;
	applyBase.lengthScale = showLength ? 1.0f / static_cast<float>( Max( settings.maxHistory, 1 ) ) : 0.0f;
	std::vector<d3d12GraphUse_t> applyUses = { { ratio, d3d12GraphAccess_t::SRV }, { colorUse, d3d12GraphAccess_t::RTV, 0 } };
	if ( left != ratio ) {
		applyUses.push_back( { left, d3d12GraphAccess_t::SRV } );
	}
	if ( showLength ) {
		applyUses.push_back( { length, d3d12GraphAccess_t::SRV } );
	}
	graph.AddPass( "rt shadow apply", d3d12GraphPass_t::RASTER, applyUses,
		[applyPso, applyBase, ratio, left, length, showLength, colorUse, rect]( idD3D12GraphContext & context ) {
			rtShadowApplyParms_t parms = applyBase;
			parms.ratioSrv = context.Srv( left );
			parms.reachSrv = context.Srv( ratio );
			parms.lengthSrv = showLength ? context.Srv( length ) : parms.reachSrv;
			const d3d12UploadSpan_t span = context.Parms( parms );
			context.Draw( *applyPso, colorUse, 0, rect, span );
		} );
	graph.Execute( groups );
}
