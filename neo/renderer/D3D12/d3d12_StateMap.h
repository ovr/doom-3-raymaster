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

#pragma once

#include "d3d12_Local.h"

/*
	samplerDesc_t → D3D12_SAMPLER_DESC, as GL_SetTexParameters sets it:

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
