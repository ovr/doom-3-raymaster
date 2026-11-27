// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#include "../../idlib/precompiled.h"

#include <bit>

#include "../tr_local.h"
#include "../BlueNoise.h"
#include "d3d12_InternalPasses.h"
#include "d3d12_Nrd.h"
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
	// distance, the roughness that reflects nothing, the fade below it, the roughness below which a pixel is a mirror
	float							limits[4];
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
	int32							first[2];
	uint32							size[2];		// texels
	uint32							noiseSrv;
	uint32							noiseIndex;
	uint32							unused[2];
};
static_assert( sizeof( rtReflectParms_t ) == 208 );

// b1 of internal/rtComposite.hlsl.
struct rtCompositeParms_t {
	float							clipToWorld[4][4];
	float							origin[4];
	float							viewport[4];
	float							limits[4];		// the roughness that reflects nothing, the fade below it, the scale times preExposure
	int32							first[2];
	uint32							reflectionSrv;
	uint32							depthSrv;
	uint32							normalSrv;
	uint32							materialSrv;
	uint32							unused[2];
};
static_assert( sizeof( rtCompositeParms_t ) == 144 );

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
	uint32							unused[3];
};
static_assert( sizeof( rtShadowApplyParms_t ) == 32 );

// b0 of internal/rtGuides.hlsl.
struct rtGuidesParms_t {
	float							clipToWorld[4][4];
	float							viewport[4];
	float							origin[4];
	int32							first[2];
	uint32							size[2];	// texels
	uint32							depthSrv;
	uint32							albedoSrv;
	uint32							guidesUav;
	uint32							slopeUav;
	uint32							stride;		// pixels a side per texel
	uint32							unused[3];
};
static_assert( sizeof( rtGuidesParms_t ) == 144 );

// b0 of internal/rtAccumulate.hlsl.
struct rtAccumulateParms_t {
	uint32							size[2];
	uint32							signalSrv;
	uint32							meanSrv;
	uint32							meanUav;
	float							weight;		// 1 over the frames in the new mean
	uint32							flags;		// bit 0: the mean before is there; 1: no rays this frame
	uint32							unused;
};
static_assert( sizeof( rtAccumulateParms_t ) == 32 );

// b0 of internal/rtIndirect.hlsl.
struct rtIndirectParms_t {
	float							clipToWorld[4][4];
	float							origin[4];
	float							viewport[4];
	float							limits[4];		// distance, what a hit emits times this, the most a ray brings (0: any)
	uint32							tlasSrv;
	uint32							recordsSrv;
	uint32							instancesSrv;
	uint32							verticesSrv;
	uint32							indexesSrv;
	uint32							skinnedSrv;
	uint32							lightsSrv;
	uint32							numLights;
	uint32							depthSrv;
	uint32							albedoSrv;
	uint32							noiseSrv;
	uint32							noiseIndex;
	uint32							amountUav;
	uint32							chromaUav;
	uint32							linearSampler;
	uint32							clampSampler;
	int32							first[2];
	uint32							size[2];		// texels
	uint32							stride;
	uint32							unused[3];
};
static_assert( sizeof( rtIndirectParms_t ) == 208 );

// b1 of internal/rtUpsample.hlsl.
struct rtUpsampleParms_t {
	float							clipToWorld[4][4];
	float							origin[4];
	float							viewport[4];
	int32							first[2];
	uint32							size[2];	// texels
	uint32							amountSrv;
	uint32							auxSrv;
	uint32							guidesSrv;
	uint32							slopeSrv;
	uint32							depthSrv;
	uint32							normalSrv;
	uint32							albedoSrv;
	uint32							stride;
	float							scale;
	uint32							flags;		// bit 0: without the albedo
	uint32							unused[2];
};
static_assert( sizeof( rtUpsampleParms_t ) == 160 );

constexpr DXGI_FORMAT GUIDES_FORMAT = DXGI_FORMAT_R32_UINT;
constexpr DXGI_FORMAT SLOPE_FORMAT = DXGI_FORMAT_R16G16_FLOAT;
// r_rtReference's means: a half float stops counting at 2048
constexpr DXGI_FORMAT MEAN_FORMAT = DXGI_FORMAT_R32G32B32A32_FLOAT;
// by rtHistorySlot_t; the graph keeps the names
constexpr std::array<const char *, static_cast<size_t>( rtHistorySlot_t::COUNT )> MEAN_NAMES = {
	"rt shadow mean", "rt indirect mean", "rt indirect chroma mean", "rt reflection mean"
};
constexpr std::array<const char *, static_cast<size_t>( rtHistorySlot_t::COUNT )> MEAN_BEFORE_NAMES = {
	"rt shadow mean before", "rt indirect mean before", "rt indirect chroma mean before", "rt reflection mean before"
};

[[nodiscard]] d3d12GraphResource_t ImportHistory( idD3D12RenderGraph & graph, const char * name, idD3D12RtHistory::surface_t & surface, uint32 width,
	uint32 height ) {
	return graph.Import( name, { surface.resource.Get(), &surface.state, surface.srv, surface.uav, {}, width, height } );
}

constexpr float REFLECTION_FADE = rtReflectionSettings_t::FADE;

// The reference's means start again when the eye moves: what the rays see depends on where it is.
[[nodiscard]] bool Moved( const rtView_t & view ) {
	return memcmp( view.worldToClip, view.prevWorldToClip, sizeof( view.worldToClip ) ) != 0;
}

// NRD's matrices are the viewport's: it all has to be in the scene.
[[nodiscard]] bool Whole( const renderRect_t & rect, const renderRect_t & viewport ) {
	return rect.x == viewport.x && rect.y == viewport.y && rect.width == viewport.width && rect.height == viewport.height;
}

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
		pso = passes.CreatePipeline( passes.Stage( ( samples > 1 ) ? "internal_rtComposite_msaa" : "internal_rtComposite" ).code,
			d3d12TargetId_t::HDR_FORMAT, samples, RGB, add ? L"doom3 rt reflection composite" : L"doom3 rt reflection alone",
			add ? d3d12PassBlend_t::ADD : d3d12PassBlend_t::REPLACE );
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

ID3D12PipelineState * idD3D12RtPasses::UpsamplePipeline( UINT samples, bool add ) {
	idComPtr<ID3D12PipelineState> & pso = upsample[( add ? 4 : 0 ) + ( std::countr_zero( samples ) & 3 )];
	if ( !pso ) {
		constexpr uint8 RGB = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
		pso = passes.CreatePipeline( passes.Stage( ( samples > 1 ) ? "internal_rtUpsample_msaa" : "internal_rtUpsample" ).code,
			d3d12TargetId_t::HDR_FORMAT, samples, RGB, add ? L"doom3 rt indirect upsample" : L"doom3 rt indirect alone",
			add ? d3d12PassBlend_t::ADD : d3d12PassBlend_t::REPLACE );
	}
	return pso.Get();
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

// A mirror's image goes from the rays to the composite as it is, the same every frame. The glossy pixels' go through
// NRD, or into r_rtReference's plain mean.
void idD3D12RtPasses::Reflections( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene, idD3D12RtHistory & history, idD3D12RayTracing & rt,
	idD3D12GpuScene & gpuScene, const rtReflectionSettings_t & settings, const rtView_t & view, const renderRect_t & viewport, float preExposure,
	idD3D12DebugGroups * groups ) {
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
	const bool multisampled = samples > 1;
	const bool glossy = settings.glossy;
	const bool reference = glossy && settings.reference;
	const bool useNrd = glossy && !reference && settings.denoiser == rtDenoiser_t::NRD && nrd != nullptr && Whole( rect, viewport );
	const d3d12ComputePipeline_t * tracePipeline = ComputePipeline( reflect[multisampled ? 1 : 0],
		multisampled ? "internal_rtReflect_msaa" : "internal_rtReflect", L"doom3 rt reflect" );
	ID3D12PipelineState * pso = CompositePipeline( samples, !settings.alone );
	if ( tracePipeline == nullptr || pso == nullptr ) {
		return;
	}

	const uint32 width = static_cast<uint32>( rect.width );
	const uint32 height = static_cast<uint32>( rect.height );
	// a frame that cannot be traced shows what NRD or the mean hold, and without them adds nothing
	const bool trace = Traceable( rt, gpuScene );
	if ( !trace && !useNrd && !reference ) {
		return;
	}

	const uint32 sceneWidth = static_cast<uint32>( scene.Width() );
	const uint32 sceneHeight = static_cast<uint32>( scene.Height() );
	const d3d12GraphResource_t colorUse = graph.Import( "SceneColor", { sceneColor.resource.Get(), &sceneColor.state, sceneColor.srv, {},
		scene.Rtv( false, true ), sceneWidth, sceneHeight } );
	const d3d12GraphResource_t depthUse = graph.Import( "Depth", { depth.resource.Get(), &depth.state, depth.srv, {}, {}, sceneWidth, sceneHeight } );
	const d3d12GraphResource_t normalUse = graph.Import( "GBuffer normal", { normal.resource.Get(), &normal.state, normal.srv, {}, {},
		sceneWidth, sceneHeight } );
	const d3d12GraphResource_t materialUse = graph.Import( "GBuffer material", { material.resource.Get(), &material.state, material.srv, {}, {},
		sceneWidth, sceneHeight } );
	d3d12PoolDesc_t desc;
	desc.format = d3d12TargetId_t::HDR_FORMAT;
	desc.width = width;
	desc.height = height;
	desc.unorderedAccess = true;
	const float mirrorRoughness = glossy ? settings.mirrorRoughness : 2.0f;

	// the frame's rays
	d3d12GraphResource_t ray;
	if ( trace ) {
		ray = graph.Create( "rt reflection", desc );
		std::vector<d3d12GraphUse_t> uses = { { depthUse, d3d12GraphAccess_t::DEPTH_READ }, { normalUse, d3d12GraphAccess_t::SRV },
			{ materialUse, d3d12GraphAccess_t::SRV }, { ray, d3d12GraphAccess_t::UAV, 0 } };
		const rtSceneImports_t imports = ImportScene( graph, rt, gpuScene, uses );
		d3d12GraphResource_t lights;
		if ( gpuScene.NumLights() > 0 ) {
			lights = graph.Import( "gpu scene lights", { gpuScene.LightBuffer(), &gpuScene.LightBufferState(), gpuScene.LightBufferSrv(), {}, {}, 0, 0 } );
			uses.push_back( { lights, d3d12GraphAccess_t::SRV } );
		}
		textures.MakeReadable( noise );

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
		base.limits[3] = mirrorRoughness;
		base.verticesSrv = rt.StaticVerticesSrv().index;
		base.indexesSrv = rt.StaticIndexesSrv().index;
		base.numLights = static_cast<uint32>( gpuScene.NumLights() );
		base.linearSampler = linearSampler;
		base.clampSampler = clampSampler;
		base.maxLights = static_cast<uint32>( Max( settings.maxLights, 0 ) );
		base.shadows = settings.shadows ? 1u : 0u;
		base.first[0] = rect.x;
		base.first[1] = rect.y;
		base.size[0] = width;
		base.size[1] = height;
		base.noiseSrv = textures.SampledView( noise, textureKind_t::TEX_2D ).index;
		// without a history every frame draws the same rays
		base.noiseIndex = settings.history ? view.sequence : 0;
		graph.AddPass( "rt reflect", d3d12GraphPass_t::COMPUTE, uses, [tracePipeline, base, depthUse, normalUse, materialUse, ray, imports,
			lights]( idD3D12GraphContext & context ) {
			rtReflectParms_t parms = base;
			parms.tlasSrv = context.Srv( imports.tlas );
			parms.recordsSrv = context.Srv( imports.records );
			parms.instancesSrv = imports.table.IsValid() ? context.Srv( imports.table ) : 0;
			parms.skinnedSrv = imports.skinned.IsValid() ? context.Srv( imports.skinned ) : 0;
			parms.lightsSrv = lights.IsValid() ? context.Srv( lights ) : 0;
			parms.depthSrv = context.Srv( depthUse );
			parms.normalSrv = context.Srv( normalUse );
			parms.materialSrv = context.Srv( materialUse );
			parms.outputUav = context.Uav( ray );
			const d3d12UploadSpan_t span = context.Parms( parms );
			context.Dispatch( *tracePipeline, &span, parms.size[0], parms.size[1] );
		} );
	}

	d3d12GraphResource_t filtered = ray;
	if ( useNrd ) {
		filtered = nrd->Reflections( graph, scene, settings, view, rect, ray ).value_or( ray );
	} else if ( reference ) {
		filtered = Accumulate( graph, history, view, rtHistorySlot_t::REFLECTION, width, height, ray, Moved( view ) );
	}
	if ( !filtered.IsValid() ) {
		graph.Execute( groups );
		return;
	}

	rtCompositeParms_t compositeBase = {};
	memcpy( compositeBase.clipToWorld, view.clipToWorld, sizeof( compositeBase.clipToWorld ) );
	compositeBase.origin[0] = view.origin[0];
	compositeBase.origin[1] = view.origin[1];
	compositeBase.origin[2] = view.origin[2];
	compositeBase.viewport[0] = static_cast<float>( viewport.x );
	compositeBase.viewport[1] = static_cast<float>( viewport.y );
	compositeBase.viewport[2] = static_cast<float>( viewport.width );
	compositeBase.viewport[3] = static_cast<float>( viewport.height );
	compositeBase.limits[0] = settings.maxRoughness;
	compositeBase.limits[1] = REFLECTION_FADE;
	compositeBase.limits[2] = settings.scale * preExposure;
	compositeBase.first[0] = rect.x;
	compositeBase.first[1] = rect.y;
	graph.AddPass( "rt composite", d3d12GraphPass_t::RASTER, { { filtered, d3d12GraphAccess_t::SRV }, { depthUse, d3d12GraphAccess_t::DEPTH_READ },
		{ normalUse, d3d12GraphAccess_t::SRV }, { materialUse, d3d12GraphAccess_t::SRV }, { colorUse, d3d12GraphAccess_t::RTV, 0 } },
		[pso, compositeBase, filtered, depthUse, normalUse, materialUse, colorUse, rect]( idD3D12GraphContext & context ) {
			rtCompositeParms_t parms = compositeBase;
			parms.reflectionSrv = context.Srv( filtered );
			parms.depthSrv = context.Srv( depthUse );
			parms.normalSrv = context.Srv( normalUse );
			parms.materialSrv = context.Srv( materialUse );
			const d3d12UploadSpan_t span = context.Parms( parms );
			context.Draw( *pso, colorUse, 0, rect, span );
		} );
	graph.Execute( groups );
}

// The guides are a texel per stride pixels of rect, also under MSAA (sample 0), for this graph only.
std::optional<idD3D12RtPasses::guideUses_t> idD3D12RtPasses::AddGuides( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene,
	const rtView_t & view, const renderRect_t & viewport, const renderRect_t & rect, uint32 stride ) {
	idD3D12SceneTarget::surface_t & depth = scene.Depth();
	idD3D12SceneTarget::surface_t & albedo = scene.GBuffer( 3 );
	if ( !depth.resource || !albedo.resource ) {
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
	const uint32 width = ( static_cast<uint32>( rect.width ) + stride - 1 ) / stride;
	const uint32 height = ( static_cast<uint32>( rect.height ) + stride - 1 ) / stride;
	d3d12PoolDesc_t desc;
	desc.format = GUIDES_FORMAT;
	desc.width = width;
	desc.height = height;
	desc.unorderedAccess = true;
	guideUses_t made;
	made.guides = graph.Create( "rt guides", desc );
	desc.format = SLOPE_FORMAT;
	made.slope = graph.Create( "rt slope", desc );

	const d3d12GraphResource_t depthUse = graph.Import( "Depth", { depth.resource.Get(), &depth.state, depth.srv, {}, {}, sceneWidth, sceneHeight } );
	const d3d12GraphResource_t albedoUse = graph.Import( "GBuffer albedo", { albedo.resource.Get(), &albedo.state, albedo.srv, {}, {},
		sceneWidth, sceneHeight } );
	rtGuidesParms_t base = {};
	memcpy( base.clipToWorld, view.clipToWorld, sizeof( base.clipToWorld ) );
	base.viewport[0] = static_cast<float>( viewport.x );
	base.viewport[1] = static_cast<float>( viewport.y );
	base.viewport[2] = static_cast<float>( viewport.width );
	base.viewport[3] = static_cast<float>( viewport.height );
	base.origin[0] = view.origin[0];
	base.origin[1] = view.origin[1];
	base.origin[2] = view.origin[2];
	base.first[0] = rect.x;
	base.first[1] = rect.y;
	base.size[0] = width;
	base.size[1] = height;
	base.stride = stride;
	graph.AddPass( "rt guides", d3d12GraphPass_t::COMPUTE, { { depthUse, d3d12GraphAccess_t::DEPTH_READ }, { albedoUse, d3d12GraphAccess_t::SRV },
		{ made.guides, d3d12GraphAccess_t::UAV, 0 }, { made.slope, d3d12GraphAccess_t::UAV, 0 } },
		[pipeline, base, depthUse, albedoUse, made]( idD3D12GraphContext & context ) {
			rtGuidesParms_t parms = base;
			parms.depthSrv = context.Srv( depthUse );
			parms.albedoSrv = context.Srv( albedoUse );
			parms.guidesUav = context.Uav( made.guides );
			parms.slopeUav = context.Uav( made.slope );
			const d3d12UploadSpan_t span = context.Parms( parms );
			context.Dispatch( *pipeline, &span, parms.size[0], parms.size[1] );
		} );
	return made;
}

// The mean is read from the frame before and written for the next one; a screenshot's (view.store false) lives for
// the graph only, and the eye's mean stays as it was.
d3d12GraphResource_t idD3D12RtPasses::Accumulate( idD3D12RenderGraph & graph, idD3D12RtHistory & history, const rtView_t & view,
	rtHistorySlot_t slot, uint32 width, uint32 height, d3d12GraphResource_t signal, bool restart ) {
	const d3d12ComputePipeline_t * pipeline = ComputePipeline( accumulate, "internal_rtAccumulate", L"doom3 rt accumulate" );
	const idD3D12RtHistory::use_t held = history.Use( slot, MEAN_FORMAT, width, height, view );
	const bool read = !restart && held.previous != nullptr;
	if ( pipeline == nullptr || ( !signal.IsValid() && !read ) ) {
		return {};
	}
	const size_t index = static_cast<size_t>( slot );
	const uint32 frames = ( read ? held.frames : 0 ) + ( signal.IsValid() ? 1 : 0 );
	d3d12GraphResource_t mean;
	if ( view.store && held.next != nullptr ) {
		mean = ImportHistory( graph, MEAN_NAMES[index], *held.next, width, height );
		history.Written( slot, view, frames );
	} else {
		d3d12PoolDesc_t desc;
		desc.format = MEAN_FORMAT;
		desc.width = width;
		desc.height = height;
		desc.unorderedAccess = true;
		mean = graph.Create( MEAN_NAMES[index], desc );
	}
	std::vector<d3d12GraphUse_t> uses = { { mean, d3d12GraphAccess_t::UAV, 0 } };
	d3d12GraphResource_t before;
	if ( read ) {
		before = ImportHistory( graph, MEAN_BEFORE_NAMES[index], *held.previous, width, height );
		uses.push_back( { before, d3d12GraphAccess_t::SRV } );
	}
	if ( signal.IsValid() ) {
		uses.push_back( { signal, d3d12GraphAccess_t::SRV } );
	}
	rtAccumulateParms_t base = {};
	base.size[0] = width;
	base.size[1] = height;
	base.weight = 1.0f / static_cast<float>( Max( frames, 1u ) );
	base.flags = ( read ? 1u : 0u ) | ( signal.IsValid() ? 0u : 2u );
	graph.AddPass( MEAN_NAMES[index], d3d12GraphPass_t::COMPUTE, uses, [pipeline, base, signal, before, mean]( idD3D12GraphContext & context ) {
		rtAccumulateParms_t parms = base;
		parms.signalSrv = signal.IsValid() ? context.Srv( signal ) : 0;
		parms.meanSrv = before.IsValid() ? context.Srv( before ) : 0;
		parms.meanUav = context.Uav( mean );
		const d3d12UploadSpan_t span = context.Parms( parms );
		context.Dispatch( *pipeline, &span, parms.size[0], parms.size[1] );
	} );
	return mean;
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
	ID3D12PipelineState * ratioPso = ShadowRatioPipeline( samples > 1 );
	ID3D12PipelineState * applyPso = ShadowApplyPipeline( samples, settings.show );
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

	// R18: what is left through NRD, or into r_rtReference's plain mean, which a frame without rays leaves as it is
	d3d12GraphResource_t left = ratio;
	if ( settings.reference ) {
		const d3d12GraphResource_t mean = Accumulate( graph, history, view, rtHistorySlot_t::SHADOW, desc.width, desc.height,
			view.held ? d3d12GraphResource_t() : ratio, Moved( view ) );
		left = mean.IsValid() ? mean : ratio;
	} else if ( settings.denoiser == rtDenoiser_t::NRD && nrd != nullptr && Whole( rect, viewport ) ) {
		left = nrd->Shadows( graph, scene, settings, view, rect, ratio, lightsUse ).value_or( ratio );
	}

	rtShadowApplyParms_t applyBase = {};
	applyBase.first[0] = rect.x;
	applyBase.first[1] = rect.y;
	applyBase.show = settings.show ? preExposure : 0.0f;
	std::vector<d3d12GraphUse_t> applyUses = { { ratio, d3d12GraphAccess_t::SRV }, { colorUse, d3d12GraphAccess_t::RTV, 0 } };
	if ( left != ratio ) {
		applyUses.push_back( { left, d3d12GraphAccess_t::SRV } );
	}
	graph.AddPass( "rt shadow apply", d3d12GraphPass_t::RASTER, applyUses, [applyPso, applyBase, ratio, left, colorUse, rect]( idD3D12GraphContext & context ) {
		rtShadowApplyParms_t parms = applyBase;
		parms.ratioSrv = context.Srv( left );
		parms.reachSrv = context.Srv( ratio );
		const d3d12UploadSpan_t span = context.Parms( parms );
		context.Draw( *applyPso, colorUse, 0, rect, span );
	} );
	graph.Execute( groups );
}

// The grid is a texel per stride pixels of rect, its block's first pixel. Its rays go through NRD, into
// r_rtReference's plain means, or as they are: the chroma is the upsample's aux as rt indirect writes it.
void idD3D12RtPasses::Indirect( idD3D12RenderGraph & graph, idD3D12SceneTarget & scene, idD3D12RtHistory & history, idD3D12RayTracing & rt,
	idD3D12GpuScene & gpuScene, const rtIndirectSettings_t & settings, const rtView_t & view, const renderRect_t & viewport, float preExposure,
	idD3D12DebugGroups * groups ) {
	idD3D12SceneTarget::surface_t & sceneColor = scene.Color( true );
	idD3D12SceneTarget::surface_t & depth = scene.Depth();
	idD3D12SceneTarget::surface_t & normal = scene.GBuffer( 0 );
	idD3D12SceneTarget::surface_t & albedo = scene.GBuffer( 3 );
	const renderRect_t rect = SceneRect( scene, viewport );
	if ( !settings.enabled || !sceneColor.resource || !depth.resource || !normal.resource || !albedo.resource
			|| rect.width <= 0 || rect.height <= 0 || viewport.width <= 0 || viewport.height <= 0 ) {
		return;
	}
	const UINT samples = scene.Samples();
	const bool multisampled = samples > 1;
	const bool reference = settings.reference;
	const bool useNrd = !reference && settings.denoiser == rtDenoiser_t::NRD && nrd != nullptr && Whole( rect, viewport );
	const d3d12ComputePipeline_t * tracePipeline = ComputePipeline( indirect[multisampled ? 1 : 0],
		multisampled ? "internal_rtIndirect_msaa" : "internal_rtIndirect", L"doom3 rt indirect" );
	ID3D12PipelineState * pso = UpsamplePipeline( samples, !settings.alone );
	if ( tracePipeline == nullptr || pso == nullptr ) {
		return;
	}

	const uint32 stride = static_cast<uint32>( Min( Max( settings.stride, 1 ), 4 ) );
	const uint32 width = ( static_cast<uint32>( rect.width ) + stride - 1 ) / stride;
	const uint32 height = ( static_cast<uint32>( rect.height ) + stride - 1 ) / stride;
	// a frame that cannot be traced takes what NRD or the means hold
	const bool trace = Traceable( rt, gpuScene );
	if ( !trace && !useNrd && !reference ) {
		return;
	}
	const std::optional<guideUses_t> guide = AddGuides( graph, scene, view, viewport, rect, stride );
	if ( !guide ) {
		return;
	}
	const guideUses_t frame = *guide;

	const uint32 sceneWidth = static_cast<uint32>( scene.Width() );
	const uint32 sceneHeight = static_cast<uint32>( scene.Height() );
	const d3d12GraphResource_t colorUse = graph.Import( "SceneColor", { sceneColor.resource.Get(), &sceneColor.state, sceneColor.srv, {},
		scene.Rtv( false, true ), sceneWidth, sceneHeight } );
	const d3d12GraphResource_t depthUse = graph.Import( "Depth", { depth.resource.Get(), &depth.state, depth.srv, {}, {}, sceneWidth, sceneHeight } );
	const d3d12GraphResource_t normalUse = graph.Import( "GBuffer normal", { normal.resource.Get(), &normal.state, normal.srv, {}, {},
		sceneWidth, sceneHeight } );
	const d3d12GraphResource_t albedoUse = graph.Import( "GBuffer albedo", { albedo.resource.Get(), &albedo.state, albedo.srv, {}, {},
		sceneWidth, sceneHeight } );
	d3d12PoolDesc_t desc;
	desc.width = width;
	desc.height = height;
	desc.unorderedAccess = true;
	desc.format = d3d12TargetId_t::HDR_FORMAT;

	// the frame's ray
	d3d12GraphResource_t ray, rayChroma;
	if ( trace ) {
		ray = graph.Create( "rt indirect ray", desc );
		rayChroma = graph.Create( "rt indirect ray chroma", desc );
		std::vector<d3d12GraphUse_t> uses = { { depthUse, d3d12GraphAccess_t::DEPTH_READ }, { albedoUse, d3d12GraphAccess_t::SRV },
			{ ray, d3d12GraphAccess_t::UAV, 0 }, { rayChroma, d3d12GraphAccess_t::UAV, 0 } };
		const rtSceneImports_t imports = ImportScene( graph, rt, gpuScene, uses );
		d3d12GraphResource_t lights;
		if ( gpuScene.NumLights() > 0 ) {
			lights = graph.Import( "gpu scene lights", { gpuScene.LightBuffer(), &gpuScene.LightBufferState(), gpuScene.LightBufferSrv(), {}, {}, 0, 0 } );
			uses.push_back( { lights, d3d12GraphAccess_t::SRV } );
		}
		textures.MakeReadable( noise );

		rtIndirectParms_t base = {};
		memcpy( base.clipToWorld, view.clipToWorld, sizeof( base.clipToWorld ) );
		base.origin[0] = view.origin[0];
		base.origin[1] = view.origin[1];
		base.origin[2] = view.origin[2];
		base.viewport[0] = static_cast<float>( viewport.x );
		base.viewport[1] = static_cast<float>( viewport.y );
		base.viewport[2] = static_cast<float>( viewport.width );
		base.viewport[3] = static_cast<float>( viewport.height );
		base.limits[0] = Max( settings.distance, 1.0f );
		base.limits[1] = Max( settings.emissiveScale, 0.0f );
		// the limit is of what the eye sees: the exposed value
		base.limits[2] = ( settings.maxRadiance > 0.0f ) ? settings.maxRadiance / Max( preExposure, 1e-6f ) : 0.0f;
		base.verticesSrv = rt.StaticVerticesSrv().index;
		base.indexesSrv = rt.StaticIndexesSrv().index;
		base.numLights = static_cast<uint32>( gpuScene.NumLights() );
		base.noiseSrv = textures.SampledView( noise, textureKind_t::TEX_2D ).index;
		// without a history every frame draws the same rays
		base.noiseIndex = settings.history ? view.sequence : 0;
		base.linearSampler = linearSampler;
		base.clampSampler = clampSampler;
		base.first[0] = rect.x;
		base.first[1] = rect.y;
		base.size[0] = width;
		base.size[1] = height;
		base.stride = stride;
		graph.AddPass( "rt indirect", d3d12GraphPass_t::COMPUTE, uses, [tracePipeline, base, depthUse, albedoUse, ray, rayChroma, imports,
			lights]( idD3D12GraphContext & context ) {
			rtIndirectParms_t parms = base;
			parms.tlasSrv = context.Srv( imports.tlas );
			parms.recordsSrv = context.Srv( imports.records );
			parms.instancesSrv = imports.table.IsValid() ? context.Srv( imports.table ) : 0;
			parms.skinnedSrv = imports.skinned.IsValid() ? context.Srv( imports.skinned ) : 0;
			parms.lightsSrv = lights.IsValid() ? context.Srv( lights ) : 0;
			parms.depthSrv = context.Srv( depthUse );
			parms.albedoSrv = context.Srv( albedoUse );
			parms.amountUav = context.Uav( ray );
			parms.chromaUav = context.Uav( rayChroma );
			const d3d12UploadSpan_t span = context.Parms( parms );
			context.Dispatch( *tracePipeline, &span, parms.size[0], parms.size[1] );
		} );
	}

	d3d12GraphResource_t filtered = ray;
	d3d12GraphResource_t spread = rayChroma;
	if ( useNrd ) {
		const std::optional<idD3D12Nrd::indirectOutput_t> denoised = nrd->Indirect( graph, scene, settings, view, rect, stride, ray, rayChroma );
		if ( denoised ) {
			filtered = denoised->amount;
			spread = denoised->aux;
		}
	} else if ( reference ) {
		const bool moved = Moved( view );
		filtered = Accumulate( graph, history, view, rtHistorySlot_t::INDIRECT_AMOUNT, width, height, ray, moved );
		spread = Accumulate( graph, history, view, rtHistorySlot_t::INDIRECT_AUX, width, height, rayChroma, moved );
	}
	if ( !filtered.IsValid() || !spread.IsValid() ) {
		graph.Execute( groups );
		return;
	}

	rtUpsampleParms_t upsampleBase = {};
	memcpy( upsampleBase.clipToWorld, view.clipToWorld, sizeof( upsampleBase.clipToWorld ) );
	upsampleBase.origin[0] = view.origin[0];
	upsampleBase.origin[1] = view.origin[1];
	upsampleBase.origin[2] = view.origin[2];
	upsampleBase.viewport[0] = static_cast<float>( viewport.x );
	upsampleBase.viewport[1] = static_cast<float>( viewport.y );
	upsampleBase.viewport[2] = static_cast<float>( viewport.width );
	upsampleBase.viewport[3] = static_cast<float>( viewport.height );
	upsampleBase.first[0] = rect.x;
	upsampleBase.first[1] = rect.y;
	upsampleBase.size[0] = width;
	upsampleBase.size[1] = height;
	upsampleBase.stride = stride;
	upsampleBase.scale = Max( settings.scale, 0.0f ) * preExposure;
	upsampleBase.flags = settings.alone ? 1u : 0u;
	graph.AddPass( "rt indirect upsample", d3d12GraphPass_t::RASTER, { { filtered, d3d12GraphAccess_t::SRV }, { spread, d3d12GraphAccess_t::SRV },
		{ frame.guides, d3d12GraphAccess_t::SRV }, { frame.slope, d3d12GraphAccess_t::SRV }, { depthUse, d3d12GraphAccess_t::DEPTH_READ },
		{ normalUse, d3d12GraphAccess_t::SRV }, { albedoUse, d3d12GraphAccess_t::SRV }, { colorUse, d3d12GraphAccess_t::RTV, 0 } },
		[pso, upsampleBase, filtered, spread, frame, depthUse, normalUse, albedoUse, colorUse, rect]( idD3D12GraphContext & context ) {
			rtUpsampleParms_t parms = upsampleBase;
			parms.amountSrv = context.Srv( filtered );
			parms.auxSrv = context.Srv( spread );
			parms.guidesSrv = context.Srv( frame.guides );
			parms.slopeSrv = context.Srv( frame.slope );
			parms.depthSrv = context.Srv( depthUse );
			parms.normalSrv = context.Srv( normalUse );
			parms.albedoSrv = context.Srv( albedoUse );
			const d3d12UploadSpan_t span = context.Parms( parms );
			context.Draw( *pso, colorUse, 0, rect, span );
		} );
	graph.Execute( groups );
}
