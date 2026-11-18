/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Dmitry Patsura

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").  

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#pragma hdrstop
#include "../../idlib/precompiled.h"

#include <string>

#include "../tr_local.h"
#include "d3d12_InternalPasses.h"
#include "d3d12_StateMap.h"

namespace {

// (-1, 1), (3, 1), (-1, -3): one triangle covers the viewport, and the scissor cuts it.
constexpr std::string_view TRIANGLE_VS = R"(
float4 vsMain( uint vertexId : SV_VertexID ) : SV_Position {
	float2 corner = float2( ( vertexId << 1 ) & 2, vertexId & 2 );
	return float4( corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0 );
}
)";

// Memory row 0 of the scene is the bottom of the screen, row 0 of a back buffer its top.
constexpr std::string_view BLIT_PS = R"(
cbuffer blitParms : register( b1 ) {
	int4 sceneSize;		// x = width, y = height
};
Texture2D<float4> scene : register( t0 );
float4 psMain( float4 position : SV_Position ) : SV_Target {
	int2 pixel = int2( position.xy );
	return scene.Load( int3( pixel.x, sceneSize.y - 1 - pixel.y, 0 ) );
}
)";

constexpr std::string_view CLEAR_PS = R"(
cbuffer clearParms : register( b1 ) {
	float4 clearColor;
};
float4 psMain() : SV_Target {
	return clearColor;
}
)";

// The panel's top row is v = 0, as GL draws an image; the target is in GL row order (row 0 at the bottom).
// Minified panels read the mip level of their scale, magnified ones the nearest texel of level 0.
constexpr std::string_view SHOW_PARMS = R"(
cbuffer showParms : register( b1 ) {
	float4 panel;		// x, y, width, height in target pixels
	float4 options;		// x: 1 = alpha as grey, y: mip level, 0 = magnified
};
SamplerState linearSampler : register( s0 );
SamplerState pointSampler : register( s1 );
float2 PanelUv( float4 position ) {
	return float2( ( position.x - panel.x ) / panel.z, 1.0 - ( position.y - panel.y ) / panel.w );
}
float4 Shown( float4 texel ) {
	return ( options.x != 0.0 ) ? float4( texel.aaa, 1.0 ) : float4( texel.rgb, 1.0 );
}
)";

constexpr std::string_view SHOW_2D_PS = R"(
Texture2D<float4> image : register( t0 );
float4 psMain( float4 position : SV_Position ) : SV_Target {
	float2 uv = PanelUv( position );
	return Shown( ( options.y > 0.0 ) ? image.SampleLevel( linearSampler, uv, options.y ) : image.SampleLevel( pointSampler, uv, 0.0 ) );
}
)";

// GL's cube face orientation, inverted: a face's (s, t) back to the direction that selects it.
constexpr std::string_view SHOW_CUBE_PS = R"(
TextureCube<float4> image : register( t1 );
float4 psMain( float4 position : SV_Position ) : SV_Target {
	float2 cells = PanelUv( position ) * float2( 4.0, 3.0 );
	int2 cell = int2( cells );
	float2 st = frac( cells ) * 2.0 - 1.0;
	float3 direction;
	if ( cell.y == 0 && cell.x == 1 ) {
		direction = float3( st.x, 1.0, st.y );			// +Y
	} else if ( cell.y == 2 && cell.x == 1 ) {
		direction = float3( st.x, -1.0, -st.y );		// -Y
	} else if ( cell.y == 1 && cell.x == 0 ) {
		direction = float3( -1.0, -st.y, st.x );		// -X
	} else if ( cell.y == 1 && cell.x == 1 ) {
		direction = float3( st.x, -st.y, 1.0 );			// +Z
	} else if ( cell.y == 1 && cell.x == 2 ) {
		direction = float3( 1.0, -st.y, -st.x );		// +X
	} else if ( cell.y == 1 && cell.x == 3 ) {
		direction = float3( -st.x, -st.y, -1.0 );		// -Z
	} else {
		discard;
		direction = float3( 0.0, 0.0, 1.0 );
	}
	return Shown( ( options.y > 0.0 ) ? image.SampleLevel( linearSampler, direction, options.y ) : image.SampleLevel( pointSampler, direction, 0.0 ) );
}
)";

// Target row r is scene row y + r, as glCopyTexImage2D lays the rows of its box out. The value is the
// one GL's GL_DEPTH_COMPONENT copy keeps: the 24-bit depth as a float, which shaders read in .x.
constexpr std::string_view DEPTH_COPY_PS = R"(
cbuffer depthCopyParms : register( b1 ) {
	int4 origin;		// x, y of the box in the scene
};
#ifdef MULTISAMPLED
Texture2DMS<float> depth : register( t0 );
#else
Texture2D<float> depth : register( t0 );
#endif
float psMain( float4 position : SV_Position ) : SV_Target {
	int2 pixel = int2( position.xy ) + origin.xy;
#ifdef MULTISAMPLED
	return depth.Load( pixel, 0 );
#else
	return depth.Load( int3( pixel, 0 ) );
#endif
}
)";

constexpr uint64 PARM_BYTES = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
constexpr int SHOW_MARGIN = 16;

[[nodiscard]] d3d12SamplerTableKey_t ShowSamplers() {
	d3d12SamplerTableKey_t key;
	key.samplers[0] = D3D12_PackSampler( { samplerFilter_t::LINEAR_MIP_LINEAR, samplerAddress_t::CLAMP, 1.0f, 0.0f } );
	key.samplers[1] = D3D12_PackSampler( { samplerFilter_t::NEAREST, samplerAddress_t::CLAMP, 1.0f, 0.0f } );
	return key;
}

// A whole magnification, or the level of a minification.
[[nodiscard]] float ShowScale( float fit, float & level ) {
	level = ( fit < 1.0f ) ? idMath::Log( 1.0f / fit ) / idMath::Log( 2.0f ) : 0.0f;
	return ( fit >= 1.0f ) ? idMath::Floor( fit ) : fit;
}

}	// namespace

idD3D12InternalPasses::idD3D12InternalPasses( const idD3D12Device & owner, const idD3D12RootSignature & signature, idD3D12ShaderCompiler & fxc,
	idD3D12Submitter & submit, idD3D12Barriers & batch, idD3D12UploadRing & constantRing, idD3D12DescriptorRing & descriptorRing,
	idD3D12SamplerHeap & samplerHeap, idD3D12DescriptorPool & srvs ) :
	device( owner ),
	root( signature ),
	compiler( fxc ),
	submitter( submit ),
	barriers( batch ),
	constants( constantRing ),
	descriptors( descriptorRing ),
	samplers( samplerHeap ),
	srvPool( srvs ) {
}

// The texts are part of the executable, so a compile error is a bug of this file.
void idD3D12InternalPasses::CreateDefaults( d3d12Descriptor_t black2D, d3d12Descriptor_t blackCube ) {
	black = { black2D, blackCube };
	showSamplers = ShowSamplers();
	const auto compile = [this]( std::string_view hlsl, shaderStageType_t stage, const char * name ) {
		auto code = compiler.Compile( hlsl, stage, name );
		if ( !code ) {
			common->FatalError( "d3d12: %s", code.error().c_str() );
			return d3d12Bytecode_t();
		}
		return std::move( *code );
	};
	triangleVs = compile( TRIANGLE_VS, shaderStageType_t::VERTEX, "d3d12 triangle" );
	blitPs = compile( BLIT_PS, shaderStageType_t::FRAGMENT, "d3d12 present blit" );
	clearPs = compile( CLEAR_PS, shaderStageType_t::FRAGMENT, "d3d12 masked clear" );
	show2DPs = compile( std::string( SHOW_PARMS ).append( SHOW_2D_PS ), shaderStageType_t::FRAGMENT, "d3d12 show image 2D" );
	showCubePs = compile( std::string( SHOW_PARMS ).append( SHOW_CUBE_PS ), shaderStageType_t::FRAGMENT, "d3d12 show image cube" );
	depthCopyPs[0] = compile( DEPTH_COPY_PS, shaderStageType_t::FRAGMENT, "d3d12 depth copy" );
	depthCopyPs[1] = compile( std::string( "#define MULTISAMPLED\n" ).append( DEPTH_COPY_PS ), shaderStageType_t::FRAGMENT, "d3d12 depth copy MSAA" );
	blit = CreatePipeline( blitPs, idD3D12SwapChain::FORMAT, 1, static_cast<uint8>( D3D12_COLOR_WRITE_ENABLE_ALL ), L"doom3 present blit" );
}

idComPtr<ID3D12PipelineState> idD3D12InternalPasses::CreatePipeline( const d3d12Bytecode_t & pixelShader, DXGI_FORMAT format, UINT samples,
	uint8 writeMask, const wchar_t * name ) const {
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
	desc.pRootSignature = root.Get();
	desc.VS = { triangleVs.data(), triangleVs.size() };
	desc.PS = { pixelShader.data(), pixelShader.size() };
	D3D12_RENDER_TARGET_BLEND_DESC & blend = desc.BlendState.RenderTarget[0];
	blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
	blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
	blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	blend.LogicOp = D3D12_LOGIC_OP_NOOP;
	blend.RenderTargetWriteMask = writeMask;
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

std::optional<idD3D12InternalPasses::room_t> idD3D12InternalPasses::TryRoom( std::span<const d3d12Descriptor_t> textures,
	const d3d12SamplerTableKey_t * samplerKey ) {
	const std::optional<d3d12UploadSpan_t> parms = constants.TryAllocate( PARM_BYTES, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT );
	if ( !parms ) {
		return std::nullopt;
	}
	room_t room = { *parms, std::nullopt, std::nullopt };
	if ( !textures.empty() ) {
		std::array<D3D12_CPU_DESCRIPTOR_HANDLE, MAX_TEXTURE_SLOTS> sources;
		for ( size_t slot = 0; slot < sources.size(); slot++ ) {
			// slot 1 is the cube of ShowImage; every other slot a pass leaves out is 2D
			const d3d12Descriptor_t filler = black[( slot == 1 ) ? 1 : 0];
			sources[slot] = srvPool.Cpu( ( slot < textures.size() && textures[slot].IsValid() ) ? textures[slot] : filler );
		}
		room.table = descriptors.TryCopyTable( sources );
		if ( !room.table ) {
			return std::nullopt;	// the parms taken here stay unused in the chunk SubmitChunk closes
		}
	}
	if ( samplerKey != nullptr ) {
		room.samplerTable = samplers.TryTable( *samplerKey, submitter.PendingFence(), submitter.WaitedFence() );
		if ( !room.samplerTable ) {
			return std::nullopt;
		}
	}
	return room;
}

idD3D12InternalPasses::room_t idD3D12InternalPasses::Room( std::span<const d3d12Descriptor_t> textures, const d3d12SamplerTableKey_t * samplerKey ) {
	std::optional<room_t> room = TryRoom( textures, samplerKey );
	while ( !room ) {
		submitter.SubmitChunk();
		uint64 oldest = Max( constants.OldestFence(), descriptors.OldestFence() );
		if ( samplerKey != nullptr && samplers.Used() == static_cast<int>( idD3D12SamplerHeap::TABLES ) ) {
			oldest = Max( oldest, samplers.OldestUse() );
		}
		if ( oldest == 0 ) {
			common->FatalError( "d3d12: an internal pass does not fit into empty rings" );
		}
		submitter.WaitFor( oldest );
		room = TryRoom( textures, samplerKey );
	}
	return *room;
}

void idD3D12InternalPasses::Record( ID3D12GraphicsCommandList1 & list, D3D12_CPU_DESCRIPTOR_HANDLE target, ID3D12PipelineState & pso,
	const renderRect_t & viewport, const room_t & room ) {
	barriers.Flush();
	list.OMSetRenderTargets( 1, &target, FALSE, nullptr );
	const D3D12_VIEWPORT view = { static_cast<float>( viewport.x ), static_cast<float>( viewport.y ),
		static_cast<float>( viewport.width ), static_cast<float>( viewport.height ), 0.0f, 1.0f };
	const D3D12_RECT scissor = { viewport.x, viewport.y, viewport.x + viewport.width, viewport.y + viewport.height };
	list.RSSetViewports( 1, &view );
	list.RSSetScissorRects( 1, &scissor );
	list.SetPipelineState( &pso );
	list.IASetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
	list.SetGraphicsRootConstantBufferView( D3D12_Root( d3d12RootSlot_t::FRAGMENT_PARMS ), room.parms.gpu );
	if ( room.table ) {
		list.SetGraphicsRootDescriptorTable( D3D12_Root( d3d12RootSlot_t::TEXTURES ), *room.table );
	}
	if ( room.samplerTable ) {
		list.SetGraphicsRootDescriptorTable( D3D12_Root( d3d12RootSlot_t::SAMPLERS ), *room.samplerTable );
	}
	list.DrawInstanced( 3, 1, 0, 0 );
}

void idD3D12InternalPasses::PresentBlit( d3d12Descriptor_t sceneSrv, D3D12_CPU_DESCRIPTOR_HANDLE backBuffer, int width, int height ) {
	const std::array textures = { sceneSrv };
	const room_t room = Room( textures );
	const int32 sceneSize[4] = { width, height, 0, 0 };
	memcpy( room.parms.cpu, sceneSize, sizeof( sceneSize ) );
	Record( submitter.List(), backBuffer, *blit.Get(), { 0, 0, width, height }, room );
}

void idD3D12InternalPasses::MaskedClear( D3D12_CPU_DESCRIPTOR_HANDLE target, d3d12TargetId_t id, const renderRect_t & rect,
	const float rgba[4], uint8 mask ) {
	assert( id.value < d3d12TargetId_t::COUNT && mask < 16 );
	idComPtr<ID3D12PipelineState> & pso = maskedClear[id.value * 16 + mask];
	if ( !pso ) {
		pso = CreatePipeline( clearPs, id.ColorView(), id.Samples(), mask, L"doom3 masked clear" );
	}
	const room_t room = Room( {} );
	memcpy( room.parms.cpu, rgba, 4 * sizeof( float ) );
	Record( submitter.List(), target, *pso.Get(), rect, room );
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
	const std::array textures = { cube ? d3d12Descriptor_t() : imageSrv, cube ? imageSrv : d3d12Descriptor_t() };
	float level = 0.0f;
	if ( cube ) {
		const int fit = Min( ( targetWidth - 2 * SHOW_MARGIN ) / 4, ( targetHeight - 2 * SHOW_MARGIN ) / 3 );
		if ( fit <= 0 ) {
			return;
		}
		const int cell = Max( 1, static_cast<int>( imageWidth * ShowScale( static_cast<float>( fit ) / imageWidth, level ) ) );
		const renderRect_t panel = { ( targetWidth - 4 * cell ) / 2, ( targetHeight - 3 * cell ) / 2, 4 * cell, 3 * cell };
		ShowPanel( *pso.Get(), textures, target, panel, false, level );
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
	ShowPanel( *pso.Get(), textures, target, { SHOW_MARGIN + ( areaWidth - width ) / 2, y, width, height }, false, level );
	ShowPanel( *pso.Get(), textures, target, { 2 * SHOW_MARGIN + areaWidth + ( areaWidth - width ) / 2, y, width, height }, true, level );
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
	const std::array textures = { depthSrv };
	const room_t room = Room( textures );
	const int32 origin[4] = { x, y, 0, 0 };
	memcpy( room.parms.cpu, origin, sizeof( origin ) );
	Record( submitter.List(), target, *pso.Get(), { 0, 0, width, height }, room );
}

void idD3D12InternalPasses::ShowPanel( ID3D12PipelineState & pso, std::span<const d3d12Descriptor_t> textures, D3D12_CPU_DESCRIPTOR_HANDLE target,
	const renderRect_t & panel, bool alpha, float level ) {
	const room_t room = Room( textures, &showSamplers );
	const float parms[8] = { static_cast<float>( panel.x ), static_cast<float>( panel.y ), static_cast<float>( panel.width ),
		static_cast<float>( panel.height ), alpha ? 1.0f : 0.0f, level, 0.0f, 0.0f };
	memcpy( room.parms.cpu, parms, sizeof( parms ) );
	Record( submitter.List(), target, pso, panel, room );
}
