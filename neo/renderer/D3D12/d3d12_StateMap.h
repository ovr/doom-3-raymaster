// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

#include <bit>

#include "d3d12_Local.h"

/*
	pipelineKey_t::target, what a draw's pass renders into, always with the scene's depth:

	  bit 0      the output's sRGB view; with bit 3 the thin G-buffer (R14), four targets
	  bits 1-2   log2 of the sample count
	  bit 3      the float HDR scene (R13), which has no sRGB view
	  bit 4      with the HDR scene: plus the light target (R16), two targets of the scene's format

	Bit 4 counts only there, and nothing reads bits 5-7: Normalized() is the id every accessor sees.
*/
struct d3d12TargetId_t {
	uint8							value = 0;
	[[nodiscard]] static constexpr d3d12TargetId_t	Of( bool srgb, UINT samples, bool hdr = false ) {	// samples 1, 2, 4 or 8
		return { static_cast<uint8>( ( ( srgb && !hdr ) ? 1 : 0 ) | ( std::countr_zero( samples ) << 1 ) | ( hdr ? 8 : 0 ) ) };
	}
	[[nodiscard]] static constexpr d3d12TargetId_t	GBuffer( UINT samples ) {
		return { static_cast<uint8>( 9 | ( std::countr_zero( samples ) << 1 ) ) };
	}
	[[nodiscard]] static constexpr d3d12TargetId_t	Lights( UINT samples ) {
		return { static_cast<uint8>( 24 | ( std::countr_zero( samples ) << 1 ) ) };
	}
	[[nodiscard]] constexpr bool	Srgb() const { return ( value & 9 ) == 1; }
	[[nodiscard]] constexpr UINT	Samples() const { return 1u << ( ( value >> 1 ) & 3 ); }
	[[nodiscard]] constexpr bool	Hdr() const { return ( value & 9 ) == 8; }		// the scene alone or with the light target
	[[nodiscard]] constexpr bool	IsGBuffer() const { return ( value & 9 ) == 9; }
	[[nodiscard]] constexpr bool	IsLights() const { return ( value & 25 ) == 24; }
	[[nodiscard]] constexpr UINT	NumTargets() const { return IsGBuffer() ? static_cast<UINT>( GBUFFER_FORMATS.size() ) : ( IsLights() ? 2 : 1 ); }
	[[nodiscard]] constexpr DXGI_FORMAT	Format( UINT target ) const { return IsGBuffer() ? GBUFFER_FORMATS[target] : ColorView(); }
	[[nodiscard]] constexpr DXGI_FORMAT	ColorView() const {
		return IsGBuffer() ? GBUFFER_FORMATS[0] : ( Hdr() ? HDR_FORMAT : ( Srgb() ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM ) );
	}
	[[nodiscard]] constexpr d3d12TargetId_t	Normalized() const { return { static_cast<uint8>( value & ( Hdr() ? 31 : 15 ) ) }; }
	constexpr bool					operator==( const d3d12TargetId_t & ) const = default;
	static constexpr int			COUNT = 32;
	static constexpr DXGI_FORMAT	DEPTH = DXGI_FORMAT_D24_UNORM_S8_UINT;
	static constexpr DXGI_FORMAT	HDR_FORMAT = DXGI_FORMAT_R16G16B16A16_FLOAT;
	// include/gbuffer.hlsli: the octahedral normal, F0 and roughness, the motion, the albedo and what was drawn
	static constexpr std::array<DXGI_FORMAT, 4>	GBUFFER_FORMATS = { DXGI_FORMAT_R16G16_SNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
		DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB };
};

/*
	samplerDesc_t → D3D12_SAMPLER_DESC, as the removed OpenGL backend's GL_SetTexParameters set it:

	  filter              D3D12 filter                                       MaxLOD
	  NEAREST             MIN_MAG_MIP_POINT                                  0 (GL samples the base level)
	  LINEAR              MIN_MAG_LINEAR_MIP_POINT                           0
	  LINEAR_MIP_NEAREST  MIN_MAG_LINEAR_MIP_POINT; anisotropy > 1:          FLT_MAX
	                      MIN_MAG_ANISOTROPIC_MIP_POINT (OPTIONS19), else ANISOTROPIC
	  LINEAR_MIP_LINEAR   MIN_MAG_MIP_LINEAR; anisotropy > 1: ANISOTROPIC    FLT_MAX

	  REPEAT → WRAP, CLAMP → CLAMP, BORDER_BLACK → BORDER (0,0,0,1), BORDER_TRANSPARENT → BORDER (0,0,0,0).

	Packed into 64 bits, the key of the sampler tables: bits 0-1 filter, 2-3 address, 4-8 anisotropy - 1,
	32-63 the float bits of lodBias. Packed 0 is NEAREST, REPEAT, no anisotropy, no bias: a valid sampler.
*/

[[nodiscard]] uint64			D3D12_PackSampler( const samplerDesc_t & sampler );
[[nodiscard]] samplerDesc_t		D3D12_UnpackSampler( uint64 packedSampler );
// anisoPointMip: OPTIONS19 has the anisotropic filter with point mips that LINEAR_MIP_NEAREST asks for.
[[nodiscard]] D3D12_SAMPLER_DESC	D3D12_SamplerFor( uint64 packedSampler, bool anisoPointMip );

/*
	pipelineKey_t → the fixed-function state of a PSO, as the removed OpenGL backend set GL
	(git show d647250:neo/renderer/OpenGL/gl_State.cpp, gl_Draw.cpp):

	  key                              D3D12
	  SRCBLEND ONE + DSTBLEND ZERO     BlendEnable FALSE, as GL disables GL_BLEND
	  a *_COLOR blend factor           its *_ALPHA form for the alpha factor: glBlendFunc's one factor covers alpha too
	  RED..ALPHAMASK (inverse)         RenderTargetWriteMask
	  DEPTHFUNC LESS ALWAYS GREATER    LESS_EQUAL ALWAYS GREATER_EQUAL EQUAL; the depth test is always on;
	    EQUAL                          raster bit 6 (strict) makes it LESS
	  DEPTHMASK                        DepthWriteMask ZERO
	  POLYMODE_LINE                    FillMode WIREFRAME
	  FUNC | front ops != 0            StencilEnable (GL's rule); read mask bits 28-35, write mask 0xFF, dynamic reference
	  INCR DECR, INCR_WRAP DECR_WRAP   INCR_SAT DECR_SAT, INCR DECR
	  SEPARATE_STENCIL                 BackFace from bits 48-56, else both faces the front ops
	  raster cull 1, 2                 CULL_MODE_FRONT, BACK with FrontCounterClockwise FALSE: the vertex shader flips
	                                   y, so GL's counter-clockwise front faces are clockwise in D3D12
	  raster bit 2 (lines)             LINE topology, and no culling, wireframe or depth bias, as GL_LINES has none
	  target                           d3d12TargetId_t: the formats and the sample count; several targets take
	                                   the one blend of the key
	  a dst alpha factor ONE           BLEND_OP_MAX for alpha into the float scene: a UNORM target clamps an
	    into the HDR scene             add at 1, and the DST_ALPHA stages after it read 0..1
	  depthBias slot                   DepthBias lround( units ), SlopeScaledDepthBias factor: D24 has GL's r = 2^-24

	D3D12_NormalizeKey clears what the description does not read, so equal normalized keys ⇔ equal PSOs.
*/

constexpr uint8						D3D12_KEY_CULL_BITS = 0x03;
constexpr uint8						D3D12_KEY_LINES = 0x04;
constexpr uint8						D3D12_KEY_STRICT_DEPTH = 0x40;
constexpr uint8						D3D12_KEY_RASTER_BITS = D3D12_KEY_CULL_BITS | D3D12_KEY_LINES | D3D12_KEY_STRICT_DEPTH;

[[nodiscard]] constexpr d3d12TargetId_t	D3D12_KeyTarget( const pipelineKey_t & key ) {
	return d3d12TargetId_t{ key.target }.Normalized();
}

struct d3d12DepthBias_t {
	INT								bias = 0;
	float							slope = 0.0f;
	bool							operator==( const d3d12DepthBias_t & ) const = default;
};
[[nodiscard]] d3d12DepthBias_t		D3D12_DepthBiasFor( float factor, float units );

// Without the bits a PSO does not depend on: the back stencil ops without SEPARATE_STENCIL (or equal to the
// front ones), the stencil state with the test off, the depth function under the strict bit, what lines lack.
[[nodiscard]] pipelineKey_t			D3D12_NormalizeKey( const pipelineKey_t & key );

// Everything of a PSO but the root signature and the shaders.
struct d3d12PipelineDesc_t {
	D3D12_BLEND_DESC				blend;
	D3D12_RASTERIZER_DESC			raster;
	D3D12_DEPTH_STENCIL_DESC1		depthStencil;
	D3D12_PRIMITIVE_TOPOLOGY_TYPE	topology;
	std::array<DXGI_FORMAT, d3d12TargetId_t::GBUFFER_FORMATS.size()>	colorFormats;	// the first numTargets, d3d12TargetId_t::Format
	uint32							numTargets;
	DXGI_SAMPLE_DESC				sample;
	std::array<D3D12_INPUT_ELEMENT_DESC, 12>	inputs;	// slot 0: what the layout has; slot 1: what it lacks
	uint32							numInputs;
	bool							defaultAttribs;		// an input reads slot 1, the buffer of DefaultAttribs()
};
// Reads only what D3D12_NormalizeKey keeps. The inputs are those the program reads, bias is the one of the
// key's slot, depthBounds says whether the device has OPTIONS2.
[[nodiscard]] d3d12PipelineDesc_t	D3D12_PipelineDescFor( const pipelineKey_t & key, uint16 programAttribMask, const d3d12DepthBias_t & bias,
										bool depthBounds );
