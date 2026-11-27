// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "../../idlib/precompiled.h"

#include <bit>
#include <cmath>
#include <cstring>

#include "../tr_local.h"
#include "d3d12_StateMap.h"

namespace {

constexpr int MAX_ANISOTROPY = 16;

constexpr uint64 UNDEFINED_STATE_BITS = ( 0x1Full << 15 ) | ( 0x1Full << 58 );	// bits 15-19 and 58-62
constexpr uint64 STENCIL_TEST_BITS = GLS_STENCIL_FUNC_BITS | GLS_STENCIL_FRONT_OP_BITS;

// By the 3-bit GLS values. GL's INCR and DECR saturate; its *_WRAP ops are D3D12's INCR and DECR.
constexpr D3D12_STENCIL_OP STENCIL_OPS[8] = {
	D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_ZERO, D3D12_STENCIL_OP_REPLACE, D3D12_STENCIL_OP_INCR_SAT,
	D3D12_STENCIL_OP_DECR_SAT, D3D12_STENCIL_OP_INVERT, D3D12_STENCIL_OP_INCR, D3D12_STENCIL_OP_DECR
};
constexpr D3D12_COMPARISON_FUNC STENCIL_FUNCS[8] = {
	D3D12_COMPARISON_FUNC_ALWAYS, D3D12_COMPARISON_FUNC_LESS, D3D12_COMPARISON_FUNC_LESS_EQUAL, D3D12_COMPARISON_FUNC_GREATER,
	D3D12_COMPARISON_FUNC_GREATER_EQUAL, D3D12_COMPARISON_FUNC_EQUAL, D3D12_COMPARISON_FUNC_NOT_EQUAL, D3D12_COMPARISON_FUNC_NEVER
};
constexpr D3D12_BLEND SOURCE_FACTORS[8] = {
	D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_DEST_COLOR, D3D12_BLEND_INV_DEST_COLOR,
	D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA
};
constexpr D3D12_BLEND DEST_FACTORS[8] = {
	D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_SRC_COLOR, D3D12_BLEND_INV_SRC_COLOR,
	D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA
};

constexpr DXGI_FORMAT ATTRIB_FORMATS[] = {	// by vertexAttribFormat_t
	DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM
};

// The VS_IN semantics of R_VertexInputs, as D3D12 names them.
struct vertexInput_t {
	int								index;			// PC_ATTRIB_INDEX_*
	const char *					semantic;
	UINT							semanticIndex;
};
constexpr vertexInput_t VERTEX_INPUTS[] = {
	{ PC_ATTRIB_INDEX_VERTEX, "POSITION", 0 },
	{ PC_ATTRIB_INDEX_ST, "TEXCOORD", 0 },
	{ PC_ATTRIB_INDEX_NORMAL, "NORMAL", 0 },
	{ PC_ATTRIB_INDEX_TANGENT, "TANGENT", 0 },
	{ PC_ATTRIB_INDEX_COLOR, "COLOR", 0 },
	{ PC_ATTRIB_INDEX_COLOR2, "COLOR", 1 },
};

[[nodiscard]] constexpr int Field( uint64 bits, uint64 mask ) {
	return static_cast<int>( ( bits & mask ) >> std::countr_zero( mask ) );
}

// D3D12 rejects a *_COLOR factor for alpha.
[[nodiscard]] constexpr D3D12_BLEND AlphaFactor( D3D12_BLEND factor ) {
	switch ( factor ) {
		case D3D12_BLEND_SRC_COLOR:			return D3D12_BLEND_SRC_ALPHA;
		case D3D12_BLEND_INV_SRC_COLOR:		return D3D12_BLEND_INV_SRC_ALPHA;
		case D3D12_BLEND_DEST_COLOR:		return D3D12_BLEND_DEST_ALPHA;
		case D3D12_BLEND_INV_DEST_COLOR:	return D3D12_BLEND_INV_DEST_ALPHA;
		default:							return factor;
	}
}

// ops: the front ops at bits 39-47, or the back ops shifted down to them.
[[nodiscard]] constexpr D3D12_DEPTH_STENCILOP_DESC StencilFace( uint64 ops, D3D12_COMPARISON_FUNC func ) {
	return { STENCIL_OPS[Field( ops, GLS_STENCIL_OP_FAIL_BITS )], STENCIL_OPS[Field( ops, GLS_STENCIL_OP_ZFAIL_BITS )],
		STENCIL_OPS[Field( ops, GLS_STENCIL_OP_PASS_BITS )], func };
}

[[nodiscard]] D3D12_COMPARISON_FUNC DepthFunc( const pipelineKey_t & key ) {
	if ( ( key.raster & D3D12_KEY_STRICT_DEPTH ) != 0 ) {
		return D3D12_COMPARISON_FUNC_LESS;
	}
	switch ( key.stateBits & GLS_DEPTHFUNC_BITS ) {
		case GLS_DEPTHFUNC_ALWAYS:	return D3D12_COMPARISON_FUNC_ALWAYS;
		case GLS_DEPTHFUNC_GREATER:	return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
		case GLS_DEPTHFUNC_EQUAL:	return D3D12_COMPARISON_FUNC_EQUAL;
		default:					return D3D12_COMPARISON_FUNC_LESS_EQUAL;
	}
}

void FillInputs( d3d12PipelineDesc_t & desc, uint8 layoutType, uint16 programAttribMask ) {
	const vertexLayoutDesc_t & layout = vertexLayouts[( layoutType < NUM_VERTEX_LAYOUTS ) ? layoutType : LAYOUT_UNKNOWN];
	for ( const vertexInput_t & input : VERTEX_INPUTS ) {
		if ( ( programAttribMask & ( 1 << input.index ) ) == 0 ) {
			continue;
		}
		D3D12_INPUT_ELEMENT_DESC & element = desc.inputs[desc.numInputs++];
		element.SemanticName = input.semantic;
		element.SemanticIndex = input.semanticIndex;
		element.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
		const std::span attribs( layout.attribs, layout.numAttribs );
		const auto attrib = std::ranges::find( attribs, input.index, &vertexAttribDesc_t::index );
		if ( attrib != attribs.end() ) {
			element.Format = ATTRIB_FORMATS[static_cast<size_t>( attrib->format )];
			element.InputSlot = 0;
			element.AlignedByteOffset = attrib->offset;
		} else {
			// GL's current value of a disabled array, (0, 0, 0, 1), from a stride-0 buffer
			element.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
			element.InputSlot = 1;
			element.AlignedByteOffset = 0;
			desc.defaultAttribs = true;
		}
	}
}

[[nodiscard]] D3D12_TEXTURE_ADDRESS_MODE AddressMode( samplerAddress_t address ) {
	switch ( address ) {
		case samplerAddress_t::CLAMP:				return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		case samplerAddress_t::BORDER_BLACK:
		case samplerAddress_t::BORDER_TRANSPARENT:	return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		default:									return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	}
}

}	// namespace

uint64 D3D12_PackSampler( const samplerDesc_t & sampler ) {
	const int anisotropy = idMath::ClampInt( 1, MAX_ANISOTROPY, static_cast<int>( sampler.maxAnisotropy + 0.5f ) );
	return static_cast<uint64>( sampler.filter ) | ( static_cast<uint64>( sampler.address ) << 2 ) | ( static_cast<uint64>( anisotropy - 1 ) << 4 )
		| ( static_cast<uint64>( std::bit_cast<uint32>( sampler.lodBias ) ) << 32 );
}

samplerDesc_t D3D12_UnpackSampler( uint64 packedSampler ) {
	samplerDesc_t sampler;
	sampler.filter = static_cast<samplerFilter_t>( packedSampler & 3 );
	sampler.address = static_cast<samplerAddress_t>( ( packedSampler >> 2 ) & 3 );
	sampler.maxAnisotropy = static_cast<float>( ( ( packedSampler >> 4 ) & 31 ) + 1 );
	sampler.lodBias = std::bit_cast<float>( static_cast<uint32>( packedSampler >> 32 ) );
	return sampler;
}

D3D12_SAMPLER_DESC D3D12_SamplerFor( uint64 packedSampler, bool anisoPointMip ) {
	const samplerDesc_t sampler = D3D12_UnpackSampler( packedSampler );
	const bool anisotropic = sampler.maxAnisotropy > 1.0f;
	D3D12_SAMPLER_DESC desc = {};
	switch ( sampler.filter ) {
		case samplerFilter_t::NEAREST:
			desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
			break;
		case samplerFilter_t::LINEAR:
			desc.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
			break;
		case samplerFilter_t::LINEAR_MIP_NEAREST:
			desc.Filter = !anisotropic ? D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT
				: ( anisoPointMip ? D3D12_FILTER_MIN_MAG_ANISOTROPIC_MIP_POINT : D3D12_FILTER_ANISOTROPIC );
			desc.MaxLOD = D3D12_FLOAT32_MAX;
			break;
		case samplerFilter_t::LINEAR_MIP_LINEAR:
			desc.Filter = anisotropic ? D3D12_FILTER_ANISOTROPIC : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			desc.MaxLOD = D3D12_FLOAT32_MAX;
			break;
	}
	desc.AddressU = desc.AddressV = desc.AddressW = AddressMode( sampler.address );
	desc.MipLODBias = idMath::ClampFloat( D3D12_MIP_LOD_BIAS_MIN, D3D12_MIP_LOD_BIAS_MAX, sampler.lodBias );
	desc.MaxAnisotropy = static_cast<UINT>( sampler.maxAnisotropy );
	// never read: no filter here compares
	desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	desc.BorderColor[3] = ( sampler.address == samplerAddress_t::BORDER_BLACK ) ? 1.0f : 0.0f;
	desc.MinLOD = 0.0f;
	return desc;
}

d3d12DepthBias_t D3D12_DepthBiasFor( float factor, float units ) {
	return { static_cast<INT>( std::lround( units ) ), factor };
}

pipelineKey_t D3D12_NormalizeKey( const pipelineKey_t & key ) {
	pipelineKey_t normal = {};
	normal.program = key.program;
	normal.layout = ( key.layout < NUM_VERTEX_LAYOUTS ) ? key.layout : static_cast<uint8>( LAYOUT_UNKNOWN );
	// the slot says it all: GLS_POLYGON_OFFSET adds nothing
	uint64 bits = key.stateBits & GLS_PIPELINE_BITS & ~( UNDEFINED_STATE_BITS | GLS_POLYGON_OFFSET );
	int raster = key.raster & D3D12_KEY_RASTER_BITS;
	if ( ( raster & D3D12_KEY_CULL_BITS ) == D3D12_KEY_CULL_BITS ) {
		raster &= ~D3D12_KEY_CULL_BITS;		// no such cull: none
	}
	if ( ( raster & D3D12_KEY_STRICT_DEPTH ) != 0 ) {
		bits &= ~GLS_DEPTHFUNC_BITS;
	}
	if ( ( raster & D3D12_KEY_LINES ) != 0 ) {
		raster &= ~D3D12_KEY_CULL_BITS;
		bits &= ~GLS_POLYMODE_LINE;
	} else {
		normal.depthBias = key.depthBias;
	}
	if ( ( bits & STENCIL_TEST_BITS ) == 0 ) {
		bits &= ~( GLS_STENCIL_FUNC_MASK_BITS | GLS_STENCIL_OP_BITS );
	} else if ( ( bits & GLS_SEPARATE_STENCIL ) == 0 || ( bits & GLS_STENCIL_BACK_OP_BITS ) == GLS_BackStencilOps( bits ) ) {
		bits &= ~( GLS_STENCIL_BACK_OP_BITS | GLS_SEPARATE_STENCIL );
	}
	normal.stateBits = bits;
	normal.raster = static_cast<uint8>( raster );
	normal.target = D3D12_KeyTarget( key ).value;
	return normal;
}

// Zeroed whole first: two descriptions of one key are equal byte for byte.
d3d12PipelineDesc_t D3D12_PipelineDescFor( const pipelineKey_t & key, uint16 programAttribMask, const d3d12DepthBias_t & bias, bool depthBounds ) {
	d3d12PipelineDesc_t desc;
	memset( &desc, 0, sizeof( desc ) );
	const uint64 bits = key.stateBits;
	const bool lines = ( key.raster & D3D12_KEY_LINES ) != 0;

	D3D12_RENDER_TARGET_BLEND_DESC target = {};
	target.BlendEnable = ( bits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) != ( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO );
	target.SrcBlend = target.BlendEnable ? SOURCE_FACTORS[Field( bits, GLS_SRCBLEND_BITS )] : D3D12_BLEND_ONE;
	target.DestBlend = target.BlendEnable ? DEST_FACTORS[Field( bits, GLS_DSTBLEND_BITS )] : D3D12_BLEND_ZERO;
	target.SrcBlendAlpha = AlphaFactor( target.SrcBlend );
	target.DestBlendAlpha = AlphaFactor( target.DestBlend );
	target.BlendOp = target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	const d3d12TargetId_t targetId = D3D12_KeyTarget( key );
	if ( targetId.Hdr() && target.BlendEnable && target.DestBlendAlpha == D3D12_BLEND_ONE ) {
		target.SrcBlendAlpha = D3D12_BLEND_ONE;
		target.BlendOpAlpha = D3D12_BLEND_OP_MAX;
	}
	target.LogicOp = D3D12_LOGIC_OP_NOOP;
	target.RenderTargetWriteMask = static_cast<UINT8>( ~Field( bits, GLS_REDMASK | GLS_GREENMASK | GLS_BLUEMASK | GLS_ALPHAMASK ) & 0xF );
	std::ranges::fill( desc.blend.RenderTarget, target );
	// R16: the light target's alpha is a sum, interaction_rt's penumbrae for NRD
	if ( targetId.IsLights() && target.BlendOpAlpha == D3D12_BLEND_OP_MAX ) {
		desc.blend.IndependentBlendEnable = TRUE;
		desc.blend.RenderTarget[1].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	}

	D3D12_RASTERIZER_DESC & raster = desc.raster;
	raster.FillMode = ( !lines && ( bits & GLS_POLYMODE_LINE ) != 0 ) ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
	const int cull = lines ? 0 : ( key.raster & D3D12_KEY_CULL_BITS );
	raster.CullMode = ( cull == 1 ) ? D3D12_CULL_MODE_FRONT : ( cull == 2 ) ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
	raster.FrontCounterClockwise = FALSE;
	if ( !lines && key.depthBias != 0 ) {
		raster.DepthBias = bias.bias;
		raster.SlopeScaledDepthBias = bias.slope;
	}
	raster.DepthClipEnable = TRUE;

	D3D12_DEPTH_STENCIL_DESC1 & depth = desc.depthStencil;
	depth.DepthEnable = TRUE;
	depth.DepthWriteMask = ( bits & GLS_DEPTHMASK ) != 0 ? D3D12_DEPTH_WRITE_MASK_ZERO : D3D12_DEPTH_WRITE_MASK_ALL;
	depth.DepthFunc = DepthFunc( key );
	depth.StencilWriteMask = 0xFF;
	if ( ( bits & STENCIL_TEST_BITS ) != 0 ) {
		const D3D12_COMPARISON_FUNC func = STENCIL_FUNCS[Field( bits, GLS_STENCIL_FUNC_BITS )];
		depth.StencilEnable = TRUE;
		depth.StencilReadMask = static_cast<UINT8>( Field( bits, GLS_STENCIL_FUNC_MASK_BITS ) );
		depth.FrontFace = StencilFace( bits, func );
		depth.BackFace = ( bits & GLS_SEPARATE_STENCIL ) != 0 ? StencilFace( bits >> GLS_STENCIL_BACK_OP_SHIFT, func ) : depth.FrontFace;
	} else {
		depth.StencilReadMask = 0xFF;
		depth.FrontFace = depth.BackFace = StencilFace( 0, D3D12_COMPARISON_FUNC_ALWAYS );
	}
	depth.DepthBoundsTestEnable = depthBounds;

	desc.topology = lines ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.numTargets = targetId.NumTargets();
	for ( uint32 i = 0; i < desc.numTargets; i++ ) {
		desc.colorFormats[i] = targetId.Format( i );
	}
	desc.sample = { targetId.Samples(), 0 };
	FillInputs( desc, key.layout, programAttribMask );
	return desc;
}
