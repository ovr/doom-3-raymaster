/*
===========================================================================

Copyright (C) 2026 Dmitry Patsura

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

Please do not use this file to train or fine-tune AI models.

===========================================================================
*/

#pragma once

/*
	The engine's Cg dialect after StripDeadCode (renderProgSource_t) → SM 5.1 HLSL. The emitter
	works on idLexer tokens and keeps the text between them, so a body reads as in the source:

	  uniform float4 rpX : register( cN );     → a member of cbuffer idVertexParms (b0) / idFragmentParms (b1), in order
	  uniform matrices_ubo { ... };            → cbuffer matrices_ubo : register( b2 )
	  uniform sampler2D sampN : register( sN ) → Texture2D sampN_t : register( tN ); SamplerState sampN_s : register( sN );
	  sampler2D x (a parameter), every x      → Texture2D x_t, SamplerState x_s / x_t, x_s
	  half*, vec2-4, fract                     → float*, float2-4, frac
	  float3( e ) with one argument            → ((float3)( e ))
	  VS_IN                                    → only the members the body reads
	  VS_OUT, PS_IN, PS_OUT                    → without semantics; PS_IN always has a position (gl_FragCoord)
	  void main( ..., out X result )           → void idMain( ..., inout X result )
	  pow( x, y )                              → idPow( x, y ): pow( abs( x ), y ) for an even whole y (E15)

	WithEntry adds the struct idVaryings of a pair and vsMain / psMain, which link it to the stage's
	structs by semantic: position, then TEXCOORD0-9, COLOR0, COLOR1, every one a float4.
*/

#include "../RenderProgSource.h"

#include <array>
#include <expected>

// The varying bits: n = TEXCOORDn.
constexpr int D3D12_VARYING_COLOR0 = 10;
constexpr int D3D12_VARYING_COLOR1 = 11;
constexpr int D3D12_NUM_VARYINGS = 12;
constexpr int D3D12_MAX_TARGETS = 8;

// A member of an entry struct; components 0 = absent.
struct d3d12StructMember_t {
	idStr							name;
	int								components = 0;
};

struct d3d12EmittedStage_t {
	idStr							body;			// prelude + the rewritten source, without an entry point
	shaderStageInfo_t				info = {};		// parms = renderProgSource_t::parms
	uint16							varyings = 0;	// VS: written, PS: read; bit n = TEXCOORDn, 10 = COLOR0, 11 = COLOR1
	uint16							attribs = 0;	// VS: PC attribute indices of the VS_IN members the body reads
	bool							usesJoints = false;	// declares matrices_ubo
	bool							writesDepth = false;
	bool							hasInput = false;	// idMain takes VS_IN / PS_IN
	d3d12StructMember_t				position;		// VS_OUT's POSITION, PS_IN's WPOS
	std::array<d3d12StructMember_t, D3D12_NUM_VARYINGS>	members;	// of VS_OUT / PS_IN by varying bit
	std::array<d3d12StructMember_t, D3D12_MAX_TARGETS>	targets;	// of PS_OUT by SV_Target index
	d3d12StructMember_t				depth;			// PS_OUT's DEPTH
};

class idD3D12HlslEmitter {
public:
	static constexpr int			VERSION = 2;	// in the blob cache key: bump it with every change of the output

	// name is the source's for the messages; an error is "name: what".
	[[nodiscard]] static std::expected<d3d12EmittedStage_t, idStr>	Emit( shaderStageType_t stage, const renderProgSource_t & source, const char * name );
	// body + an idVaryings with exactly the members in varyings + vsMain or psMain. A varying the
	// VS does not write is 0, one the PS does not read is not declared.
	[[nodiscard]] static idStr		WithEntry( shaderStageType_t stage, const d3d12EmittedStage_t & emitted, uint16 varyings );
};
