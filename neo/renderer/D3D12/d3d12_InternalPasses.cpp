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

#include "../tr_local.h"
#include "d3d12_InternalPasses.h"

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

constexpr uint64 PARM_BYTES = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;

}	// namespace

idD3D12InternalPasses::idD3D12InternalPasses( const idD3D12Device & owner, const idD3D12RootSignature & signature, idD3D12ShaderCompiler & fxc,
	idD3D12Submitter & submit, idD3D12Barriers & batch, idD3D12UploadRing & constantRing, idD3D12DescriptorRing & descriptorRing,
	idD3D12DescriptorPool & srvs ) :
	device( owner ),
	root( signature ),
	compiler( fxc ),
	submitter( submit ),
	barriers( batch ),
	constants( constantRing ),
	descriptors( descriptorRing ),
	srvPool( srvs ),
	nullSrv( srvs.Allocate() ) {
}

// The texts are part of the executable, so a compile error is a bug of this file.
void idD3D12InternalPasses::CreateDefaults() {
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
	blit = CreatePipeline( blitPs, idD3D12SwapChain::FORMAT, 1, static_cast<uint8>( D3D12_COLOR_WRITE_ENABLE_ALL ), L"doom3 present blit" );

	D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
	view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	view.Texture2D.MipLevels = 1;
	device.Get().CreateShaderResourceView( nullptr, &view, srvPool.Cpu( nullSrv ) );
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

std::optional<idD3D12InternalPasses::room_t> idD3D12InternalPasses::TryRoom( std::span<const d3d12Descriptor_t> textures ) {
	const std::optional<d3d12UploadSpan_t> parms = constants.TryAllocate( PARM_BYTES, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT );
	if ( !parms ) {
		return std::nullopt;
	}
	room_t room = { *parms, std::nullopt };
	if ( !textures.empty() ) {
		std::array<D3D12_CPU_DESCRIPTOR_HANDLE, MAX_TEXTURE_SLOTS> sources;
		for ( size_t slot = 0; slot < sources.size(); slot++ ) {
			sources[slot] = srvPool.Cpu( slot < textures.size() ? textures[slot] : nullSrv );
		}
		room.table = descriptors.TryCopyTable( sources );
		if ( !room.table ) {
			return std::nullopt;	// the parms taken here stay unused in the chunk SubmitChunk closes
		}
	}
	return room;
}

idD3D12InternalPasses::room_t idD3D12InternalPasses::Room( std::span<const d3d12Descriptor_t> textures ) {
	std::optional<room_t> room = TryRoom( textures );
	while ( !room ) {
		submitter.SubmitChunk();
		const uint64 oldest = Max( constants.OldestFence(), descriptors.OldestFence() );
		if ( oldest == 0 ) {
			common->FatalError( "d3d12: an internal pass does not fit into empty rings" );
		}
		submitter.WaitFor( oldest );
		room = TryRoom( textures );
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
