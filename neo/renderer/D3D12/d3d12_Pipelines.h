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

#include "d3d12_Device.h"

/*
	The one root signature of every program and every internal pass, version 1.1, 8 DWORDs:

	  0  VERTEX_PARMS    root CBV b0, vertex shader     the program's vertex parms (GL's _va_[])
	  1  FRAGMENT_PARMS  root CBV b1, pixel shader      the program's fragment parms (GL's _fa_[])
	  2  JOINTS          root CBV b2, vertex shader     the joint block at the draw's joint offset
	  3  TEXTURES        table t0-t7, pixel shader      one SRV per texture slot, data volatile
	  4  SAMPLERS        table s0-s7, pixel shader      the sampler of each slot
*/
class idD3D12RootSignature {
public:
									idD3D12RootSignature( const idD3D12Device & device, const idD3D12Library & library );
	[[nodiscard]] ID3D12RootSignature *	Get() const { return signature.Get(); }

private:
	idComPtr<ID3D12RootSignature>	signature;
};
