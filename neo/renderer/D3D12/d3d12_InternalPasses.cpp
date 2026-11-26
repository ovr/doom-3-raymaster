// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "../../idlib/precompiled.h"

#include "../tr_local.h"
#include "d3d12_InternalPasses.h"
#include "d3d12_StateMap.h"

namespace {

constexpr uint64 PARM_BYTES = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
constexpr int SHOW_MARGIN = 16;
constexpr DXGI_FORMAT GAMMA_LUT_FORMAT = DXGI_FORMAT_R16G16B16A16_UNORM;
constexpr UINT GAMMA_LUT_ROW_BYTES = 256 * 4 * sizeof( uint16 );	// one row, already a multiple of the 256-byte pitch

// A whole magnification, or the level of a minification.
[[nodiscard]] float ShowScale( float fit, float & level ) {
	level = ( fit < 1.0f ) ? idMath::Log( 1.0f / fit ) / idMath::Log( 2.0f ) : 0.0f;
	return ( fit >= 1.0f ) ? idMath::Floor( fit ) : fit;
}

}	// namespace

d3d12GammaRamp_t D3D12_IdentityGamma() {
	d3d12GammaRamp_t ramp;
	for ( uint16 i = 0; i < 256; i++ ) {
		ramp.red[i] = ramp.green[i] = ramp.blue[i] = static_cast<uint16>( i * 257 );
	}
	return ramp;
}

bool D3D12_GammaIsIdentity( const d3d12GammaRamp_t & ramp ) {
	for ( const std::array<uint16, 256> * channel : { &ramp.red, &ramp.green, &ramp.blue } ) {
		for ( uint32 i = 0; i < 256; i++ ) {
			// no 16-bit value lies halfway between two bytes, so this rounding is the GPU's
			if ( ( ( *channel )[i] * 255u + 32767u ) / 65535u != i ) {
				return false;
			}
		}
	}
	return true;
}

idD3D12InternalPasses::idD3D12InternalPasses( const idD3D12Device & owner, const idD3D12RootSignature & signature, idD3D12HlslCompiler & dxc,
	idD3D12Submitter & submit, idD3D12Barriers & batch, idD3D12Uploader & copies, idD3D12UploadRing & constantRing,
	idD3D12SamplerHeap & samplerHeap, idD3D12DescriptorPool & srvs ) :
	device( owner ),
	root( signature ),
	compiler( dxc ),
	submitter( submit ),
	barriers( batch ),
	uploader( copies ),
	constants( constantRing ),
	samplers( samplerHeap ),
	srvPool( srvs ) {
}

namespace {

constexpr d3d12InternalStage_t INTERNAL_STAGES[] = {
	{ "triangle", shaderStageType_t::VERTEX, "internal_triangle", nullptr },
	{ "blit", shaderStageType_t::FRAGMENT, "internal_blit", nullptr },
	{ "blit", shaderStageType_t::FRAGMENT, "internal_blit_gamma", "GAMMA=1" },
	{ "clear", shaderStageType_t::FRAGMENT, "internal_clear", nullptr },
	{ "showImage", shaderStageType_t::FRAGMENT, "internal_showImage", nullptr },
	{ "showImage", shaderStageType_t::FRAGMENT, "internal_showImage_cube", "CUBE=1" },
	{ "depthCopy", shaderStageType_t::FRAGMENT, "internal_depthCopy", nullptr },
	{ "depthCopy", shaderStageType_t::FRAGMENT, "internal_depthCopy_msaa", "MULTISAMPLED=1" },
	{ "tonemap", shaderStageType_t::FRAGMENT, "internal_tonemap", nullptr },
	{ "combineLut", shaderStageType_t::COMPUTE, "internal_combineLut", nullptr },
	{ "hdrResolve", shaderStageType_t::COMPUTE, "internal_hdrResolve", nullptr },
	{ "exposureHistogram", shaderStageType_t::COMPUTE, "internal_exposureHistogram", nullptr },
	{ "exposureAdapt", shaderStageType_t::COMPUTE, "internal_exposureAdapt", nullptr },
	{ "bloomDownsample", shaderStageType_t::COMPUTE, "internal_bloomDownsample", nullptr },
	{ "bloomBlur", shaderStageType_t::COMPUTE, "internal_bloomBlur", nullptr },
	{ "localExposureGrid", shaderStageType_t::COMPUTE, "internal_localExposureGrid", nullptr },
	{ "localExposureBlur", shaderStageType_t::COMPUTE, "internal_localExposureBlur", nullptr },
	{ "hdrEncode", shaderStageType_t::FRAGMENT, "internal_hdrEncode", nullptr },
	{ "hdrEncode", shaderStageType_t::FRAGMENT, "internal_hdrEncode_msaa", "MULTISAMPLED=1" },
	{ "gbufferDebug", shaderStageType_t::FRAGMENT, "internal_gbufferDebug", nullptr },
	{ "gbufferDebug", shaderStageType_t::FRAGMENT, "internal_gbufferDebug_msaa", "MULTISAMPLED=1" },
	{ "rtSkin", shaderStageType_t::COMPUTE, "internal_rtSkin", nullptr },
	{ "rtDebug", shaderStageType_t::FRAGMENT, "internal_rtDebug", nullptr },
	{ "rtDebug", shaderStageType_t::FRAGMENT, "internal_rtDebug_msaa", "MULTISAMPLED=1" },
	{ "rtReflect", shaderStageType_t::COMPUTE, "internal_rtReflect", nullptr },
	{ "rtReflect", shaderStageType_t::COMPUTE, "internal_rtReflect_msaa", "MULTISAMPLED=1" },
	{ "rtComposite", shaderStageType_t::FRAGMENT, "internal_rtComposite", nullptr },
	{ "rtShadowRatio", shaderStageType_t::FRAGMENT, "internal_rtShadowRatio", nullptr },
	{ "rtShadowRatio", shaderStageType_t::FRAGMENT, "internal_rtShadowRatio_msaa", "MULTISAMPLED=1" },
	{ "rtShadowApply", shaderStageType_t::FRAGMENT, "internal_rtShadowApply", nullptr },
	{ "rtGuides", shaderStageType_t::COMPUTE, "internal_rtGuides", nullptr },
	{ "rtGuides", shaderStageType_t::COMPUTE, "internal_rtGuides_msaa", "MULTISAMPLED=1" },
	{ "rtTemporal", shaderStageType_t::COMPUTE, "internal_rtTemporal", nullptr },
	{ "rtAtrous", shaderStageType_t::COMPUTE, "internal_rtAtrous", nullptr },
};

}	// namespace

std::span<const d3d12InternalStage_t> D3D12_InternalStages() {
	return INTERNAL_STAGES;
}

std::optional<d3d12HlslSource_t> D3D12_InternalSource( const d3d12InternalStage_t & stage, const d3d12ShaderReader_t & reader ) {
	std::optional<d3d12HlslSource_t> source = D3D12_LoadHlslFile( va( "shaders/internal/%s.hlsl", stage.file ), stage.name, reader );
	if ( source && stage.define != nullptr ) {
		source->defines.Append( stage.define );
	}
	return source;
}

// shaders/internal/*.hlsl: without them the backend cannot present, so a missing or broken file is fatal.
void idD3D12InternalPasses::CreateDefaults() {
	// held for the context's life
	showSamplers[0] = samplers.Acquire( D3D12_PackSampler( { samplerFilter_t::LINEAR_MIP_LINEAR, samplerAddress_t::CLAMP, 1.0f, 0.0f } ) );
	showSamplers[1] = samplers.Acquire( D3D12_PackSampler( { samplerFilter_t::NEAREST, samplerAddress_t::CLAMP, 1.0f, 0.0f } ) );
	// in the order of INTERNAL_STAGES; the stages after them are for Stage()
	d3d12Bytecode_t * const codes[] = { &triangleVs, &blitPs, &blitGammaPs, &clearPs, &show2DPs, &showCubePs, &depthCopyPs[0], &depthCopyPs[1] };
	static_assert( std::size( codes ) <= std::size( INTERNAL_STAGES ) );
	std::vector<d3d12HlslSource_t> sources;
	for ( const d3d12InternalStage_t & stage : INTERNAL_STAGES ) {
		std::optional<d3d12HlslSource_t> source = D3D12_InternalSource( stage, compiler.Reader() );
		if ( !source ) {
			common->FatalError( "d3d12: shaders/internal/%s.hlsl is missing: copy base/shaders into the game folder", stage.file );
		}
		sources.push_back( std::move( *source ) );
	}
	std::vector<d3d12HlslRequest_t> requests;
	for ( size_t i = 0; i < sources.size(); i++ ) {
		requests.push_back( { &sources[i], INTERNAL_STAGES[i].type, hlslParms_t::ANY } );
	}
	std::vector<d3d12HlslResult_t> built = compiler.BuildAll( requests );
	stages.clear();
	for ( size_t i = 0; i < built.size(); i++ ) {
		if ( !built[i] ) {
			common->FatalError( "d3d12: %s", built[i].error().c_str() );
		}
		if ( i < std::size( codes ) ) {
			*codes[i] = built[i]->code;
		}
		stages.push_back( std::move( *built[i] ) );
	}
	blit = CreatePipeline( blitPs, idD3D12SwapChain::FORMAT, 1, static_cast<uint8>( D3D12_COLOR_WRITE_ENABLE_ALL ), L"doom3 present blit" );
}

const d3d12HlslStage_t & idD3D12InternalPasses::Stage( const char * name ) const {
	for ( size_t i = 0; i < stages.size(); i++ ) {
		if ( idStr::Cmp( INTERNAL_STAGES[i].name, name ) == 0 ) {
			return stages[i];
		}
	}
	common->FatalError( "d3d12: no internal stage %s", name );
	static const d3d12HlslStage_t none;
	return none;
}

idComPtr<ID3D12PipelineState> idD3D12InternalPasses::CreatePipeline( const d3d12Bytecode_t & pixelShader, DXGI_FORMAT format, UINT samples,
	uint8 writeMask, const wchar_t * name, d3d12PassBlend_t blend ) const {
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
	desc.pRootSignature = root.Get();
	desc.VS = { triangleVs.data(), triangleVs.size() };
	desc.PS = { pixelShader.data(), pixelShader.size() };
	const bool multiply = ( blend == d3d12PassBlend_t::MULTIPLY );
	D3D12_RENDER_TARGET_BLEND_DESC & target = desc.BlendState.RenderTarget[0];
	target.BlendEnable = ( blend != d3d12PassBlend_t::REPLACE );
	target.SrcBlend = target.SrcBlendAlpha = multiply ? D3D12_BLEND_ZERO : D3D12_BLEND_ONE;
	target.DestBlend = multiply ? D3D12_BLEND_SRC_COLOR : ( target.BlendEnable ? D3D12_BLEND_ONE : D3D12_BLEND_ZERO );
	target.DestBlendAlpha = multiply ? D3D12_BLEND_SRC_ALPHA : target.DestBlend;
	target.BlendOp = target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	target.LogicOp = D3D12_LOGIC_OP_NOOP;
	target.RenderTargetWriteMask = writeMask;
	desc.SampleMask = UINT_MAX;
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	desc.RasterizerState.DepthClipEnable = TRUE;
	const D3D12_DEPTH_STENCILOP_DESC keep = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS };
	desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	desc.DepthStencilState.FrontFace = keep;
	desc.DepthStencilState.BackFace = keep;
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = format;
	desc.SampleDesc.Count = samples;
	idComPtr<ID3D12PipelineState> pso;
	if ( device.Check( device.Get().CreateGraphicsPipelineState( &desc, IID_PPV_ARGS( &pso ) ), "CreateGraphicsPipelineState (internal pass)" ) ) {
		pso->SetName( name );
	}
	return pso;
}

d3d12UploadSpan_t idD3D12InternalPasses::Parms() {
	std::optional<d3d12UploadSpan_t> parms = constants.TryAllocate( PARM_BYTES, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT );
	while ( !parms ) {
		submitter.SubmitChunk();
		const uint64 oldest = constants.OldestFence();
		if ( oldest == 0 ) {
			common->FatalError( "d3d12: an internal pass does not fit into an empty ring" );
		}
		submitter.WaitFor( oldest );
		parms = constants.TryAllocate( PARM_BYTES, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT );
	}
	return *parms;
}

void idD3D12InternalPasses::Record( ID3D12GraphicsCommandList4 & list, D3D12_CPU_DESCRIPTOR_HANDLE target, ID3D12PipelineState & pso,
	const renderRect_t & viewport, const d3d12UploadSpan_t & parms, const d3d12DrawResources_t * resources ) {
	barriers.Flush();
	list.OMSetRenderTargets( 1, &target, FALSE, nullptr );
	const D3D12_VIEWPORT view = { static_cast<float>( viewport.x ), static_cast<float>( viewport.y ),
		static_cast<float>( viewport.width ), static_cast<float>( viewport.height ), 0.0f, 1.0f };
	const D3D12_RECT scissor = { viewport.x, viewport.y, viewport.x + viewport.width, viewport.y + viewport.height };
	list.RSSetViewports( 1, &view );
	list.RSSetScissorRects( 1, &scissor );
	list.SetPipelineState( &pso );
	list.IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
	list.SetGraphicsRootConstantBufferView( D3D12_Root( d3d12RootSlot_t::FRAGMENT_PARMS ), parms.gpu );
	if ( resources != nullptr ) {
		list.SetGraphicsRoot32BitConstants( D3D12_Root( d3d12RootSlot_t::DRAW_RESOURCES ), d3d12DrawResources_t::COUNT, resources->indices.data(), 0 );
	}
	list.DrawInstanced( 3, 1, 0, 0 );
}

void idD3D12InternalPasses::PresentBlit( d3d12Descriptor_t sceneSrv, D3D12_CPU_DESCRIPTOR_HANDLE backBuffer, int width, int height ) {
	const bool mapped = !gammaIdentity && blitGamma;
	if ( mapped ) {
		barriers.Transition( *gammaLut.Get(), gammaLutState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
	}
	d3d12DrawResources_t resources;
	resources.SetTexture( 0, false, sceneSrv.index );
	if ( mapped ) {
		resources.SetTexture( 1, false, gammaLutSrv.index );
	}
	const d3d12UploadSpan_t parms = Parms();
	const int32 sceneSize[4] = { width, height, 0, 0 };
	memcpy( parms.cpu, sceneSize, sizeof( sceneSize ) );
	Record( submitter.List(), backBuffer, mapped ? *blitGamma.Get() : *blit.Get(), { 0, 0, width, height }, parms, &resources );
}

// The upload goes into the open list, behind every blit that read the old ramp.
void idD3D12InternalPasses::SetGamma( const d3d12GammaRamp_t & ramp ) {
	if ( ramp == gamma ) {
		return;
	}
	gamma = ramp;
	gammaIdentity = D3D12_GammaIsIdentity( ramp );
	if ( gammaIdentity || ( !gammaLut && !CreateGammaLut() ) ) {
		return;		// an identity ramp never reads the LUT, so it waits for the next ramp that does
	}
	const std::optional<d3d12UploadSpan_t> span = uploader.Stage( GAMMA_LUT_ROW_BYTES, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT );
	if ( !span ) {
		return;
	}
	std::array<uint16, 256 * 4> texels;
	for ( size_t i = 0; i < 256; i++ ) {
		texels[i * 4 + 0] = ramp.red[i];
		texels[i * 4 + 1] = ramp.green[i];
		texels[i * 4 + 2] = ramp.blue[i];
		texels[i * 4 + 3] = 0xFFFF;
	}
	memcpy( span->cpu, texels.data(), sizeof( texels ) );
	const D3D12_SUBRESOURCE_FOOTPRINT footprint = { GAMMA_LUT_FORMAT, 256, 1, 1, GAMMA_LUT_ROW_BYTES };
	uploader.TextureRegion( *gammaLut.Get(), gammaLutState, 0, *span, footprint, 1 );
}

bool idD3D12InternalPasses::CreateGammaLut() {
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = 256;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = GAMMA_LUT_FORMAT;
	desc.SampleDesc.Count = 1;
	const D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
	if ( !device.Check( device.Get().CreateCommittedResource( &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
			IID_PPV_ARGS( &gammaLut ) ), "CreateCommittedResource (gamma LUT)" ) ) {
		return false;
	}
	gammaLut->SetName( L"doom3 gamma LUT" );
	gammaLutState = {};
	gammaLutState.current = D3D12_RESOURCE_STATE_COPY_DEST;
	D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
	view.Format = GAMMA_LUT_FORMAT;
	view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	view.Texture2D.MipLevels = 1;
	gammaLutSrv = srvPool.Allocate();
	srvPool.CreateSrv( gammaLutSrv, *gammaLut.Get(), view );
	blitGamma = CreatePipeline( blitGammaPs, idD3D12SwapChain::FORMAT, 1, static_cast<uint8>( D3D12_COLOR_WRITE_ENABLE_ALL ),
		L"doom3 present blit with gamma" );
	return blitGamma != nullptr;
}

void idD3D12InternalPasses::MaskedClear( D3D12_CPU_DESCRIPTOR_HANDLE target, d3d12TargetId_t id, const renderRect_t & rect,
	const float rgba[4], uint8 mask ) {
	assert( id.value < d3d12TargetId_t::COUNT && mask < 16 );
	idComPtr<ID3D12PipelineState> & pso = maskedClear[id.value * 16 + mask];
	if ( !pso ) {
		pso = CreatePipeline( clearPs, id.ColorView(), id.Samples(), mask, L"doom3 masked clear" );
	}
	const d3d12UploadSpan_t parms = Parms();
	memcpy( parms.cpu, rgba, 4 * sizeof( float ) );
	Record( submitter.List(), target, *pso.Get(), rect, parms, nullptr );
}

void idD3D12InternalPasses::ShowImage( d3d12Descriptor_t imageSrv, textureKind_t kind, int imageWidth, int imageHeight,
	D3D12_CPU_DESCRIPTOR_HANDLE target, d3d12TargetId_t id, int targetWidth, int targetHeight ) {
	assert( id.value < d3d12TargetId_t::COUNT );
	const bool cube = ( kind == textureKind_t::TEX_CUBE );
	idComPtr<ID3D12PipelineState> & pso = showImage[id.value * 2 + ( cube ? 1 : 0 )];
	if ( !pso ) {
		pso = CreatePipeline( cube ? showCubePs : show2DPs, id.ColorView(), id.Samples(), static_cast<uint8>( D3D12_COLOR_WRITE_ENABLE_ALL ),
			L"doom3 show image" );
	}
	d3d12DrawResources_t resources;
	resources.SetTexture( cube ? 1 : 0, cube, imageSrv.index );
	resources.SetSampler( 0, showSamplers[0] );
	resources.SetSampler( 1, showSamplers[1] );
	float level = 0.0f;
	if ( cube ) {
		const int fit = Min( ( targetWidth - 2 * SHOW_MARGIN ) / 4, ( targetHeight - 2 * SHOW_MARGIN ) / 3 );
		if ( fit <= 0 ) {
			return;
		}
		const int cell = Max( 1, static_cast<int>( imageWidth * ShowScale( static_cast<float>( fit ) / imageWidth, level ) ) );
		const renderRect_t panel = { ( targetWidth - 4 * cell ) / 2, ( targetHeight - 3 * cell ) / 2, 4 * cell, 3 * cell };
		ShowPanel( *pso.Get(), resources, target, panel, false, level );
		return;
	}
	const int areaWidth = ( targetWidth - 3 * SHOW_MARGIN ) / 2;
	const int areaHeight = targetHeight - 2 * SHOW_MARGIN;
	if ( areaWidth <= 0 || areaHeight <= 0 ) {
		return;
	}
	const float fit = Min( static_cast<float>( areaWidth ) / imageWidth, static_cast<float>( areaHeight ) / imageHeight );
	const float scale = ShowScale( fit, level );
	const int width = Max( 1, static_cast<int>( imageWidth * scale ) );
	const int height = Max( 1, static_cast<int>( imageHeight * scale ) );
	const int y = SHOW_MARGIN + ( areaHeight - height ) / 2;
	ShowPanel( *pso.Get(), resources, target, { SHOW_MARGIN + ( areaWidth - width ) / 2, y, width, height }, false, level );
	ShowPanel( *pso.Get(), resources, target, { 2 * SHOW_MARGIN + areaWidth + ( areaWidth - width ) / 2, y, width, height }, true, level );
}

void idD3D12InternalPasses::CopyDepth( d3d12Descriptor_t depthSrv, UINT samples, D3D12_CPU_DESCRIPTOR_HANDLE target, int x, int y, int width,
	int height ) {
	const size_t multisampled = ( samples > 1 ) ? 1 : 0;
	idComPtr<ID3D12PipelineState> & pso = depthCopy[multisampled];
	if ( !pso ) {
		pso = CreatePipeline( depthCopyPs[multisampled], DXGI_FORMAT_R32_FLOAT, 1, static_cast<uint8>( D3D12_COLOR_WRITE_ENABLE_ALL ),
			L"doom3 depth copy" );
		if ( !pso ) {
			return;		// a lost device
		}
	}
	d3d12DrawResources_t resources;
	resources.SetTexture( 0, false, depthSrv.index );
	const d3d12UploadSpan_t parms = Parms();
	const int32 origin[4] = { x, y, 0, 0 };
	memcpy( parms.cpu, origin, sizeof( origin ) );
	Record( submitter.List(), target, *pso.Get(), { 0, 0, width, height }, parms, &resources );
}

void idD3D12InternalPasses::GBufferDebug( std::span<const d3d12Descriptor_t, idD3D12SceneTarget::GBUFFER_TARGETS> gbufferSrvs, UINT samples,
	gbufferDebug_t mode, D3D12_CPU_DESCRIPTOR_HANDLE target, d3d12TargetId_t id, const renderRect_t & viewport ) {
	assert( id.value < d3d12TargetId_t::COUNT && id.Samples() == samples );
	idComPtr<ID3D12PipelineState> & pso = gbufferDebug[id.value];
	if ( !pso ) {
		const char * stage = ( samples > 1 ) ? "internal_gbufferDebug_msaa" : "internal_gbufferDebug";
		pso = CreatePipeline( Stage( stage ).code, id.ColorView(), id.Samples(), static_cast<uint8>( D3D12_COLOR_WRITE_ENABLE_ALL ),
			L"doom3 G-buffer debug" );
		if ( !pso ) {
			return;		// a lost device
		}
	}
	const d3d12UploadSpan_t parms = Parms();
	std::array<uint32, idD3D12SceneTarget::GBUFFER_TARGETS + 1> values;
	for ( size_t i = 0; i < gbufferSrvs.size(); i++ ) {
		values[i] = gbufferSrvs[i].index;
	}
	values.back() = static_cast<uint32>( mode );
	memcpy( parms.cpu, values.data(), sizeof( values ) );
	Record( submitter.List(), target, *pso.Get(), viewport, parms, nullptr );
}

void idD3D12InternalPasses::ShowPanel( ID3D12PipelineState & pso, const d3d12DrawResources_t & resources, D3D12_CPU_DESCRIPTOR_HANDLE target,
	const renderRect_t & panel, bool alpha, float level ) {
	const d3d12UploadSpan_t parms = Parms();
	const float values[8] = { static_cast<float>( panel.x ), static_cast<float>( panel.y ), static_cast<float>( panel.width ),
		static_cast<float>( panel.height ), alpha ? 1.0f : 0.0f, level, 0.0f, 0.0f };
	memcpy( parms.cpu, values, sizeof( values ) );
	Record( submitter.List(), target, pso, panel, parms, &resources );
}
