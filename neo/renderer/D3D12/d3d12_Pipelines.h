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

#include "d3d12_Programs.h"
#include "d3d12_StateMap.h"

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

// pipelineKey_t::depthBias → a D3D12 bias. Each slot holds a distinct bias and none holds no bias, so
// equal slots ⇔ equal PSOs; slots live as long as the backend.
class idD3D12DepthBiasTable {
public:
	// 0 for no bias. A full table warns once and gives the nearest bias it has, no bias included.
	[[nodiscard]] uint8				Slot( float factor, float units );
	[[nodiscard]] d3d12DepthBias_t	operator[]( uint8 slot ) const;		// slot 0: none
	[[nodiscard]] int				Num() const { return count; }
	[[nodiscard]] int				NearestThisFrame() const { return nearest; }	// draws whose own bias did not fit
	void							ResetFrameCounters() { nearest = 0; }

private:
	[[nodiscard]] uint8				NearestSlot( const d3d12DepthBias_t & wanted ) const;

	std::array<d3d12DepthBias_t, 255>	biases = {};	// slot - 1
	int								count = 0;
	float							lastFactor = 0.0f;	// shared code sets one pair for many draws
	float							lastUnits = 0.0f;
	uint8							lastSlot = 0;
	bool							lastNearest = false;
	bool							reportedFull = false;
	int								nearest = 0;
};

// The PSOs of every program, by normalized key, created on first use; they die with their program.
class idD3D12PipelineFactory {
public:
									idD3D12PipelineFactory( const idD3D12Device & device, const idD3D12RootSignature & root );
	// nullptr only on a lost device.
	[[nodiscard]] ID3D12PipelineState *	Get( d3d12Program_t & program, const pipelineKey_t & key );
	[[nodiscard]] idD3D12DepthBiasTable &	Bias() { return bias; }
	[[nodiscard]] const idD3D12DepthBiasTable &	Bias() const { return bias; }
	[[nodiscard]] int				CreatedThisFrame() const { return created; }
	[[nodiscard]] int				Num() const { return total; }
	void							ResetFrameCounters() { created = 0; bias.ResetFrameCounters(); }

private:
	[[nodiscard]] idComPtr<ID3D12PipelineState>	Create( const d3d12Program_t & program, const pipelineKey_t & key ) const;

	const idD3D12Device &			device;
	const idD3D12RootSignature &	root;
	idD3D12DepthBiasTable			bias;
	int								created = 0;
	int								total = 0;		// created since Init; DestroyProgram does not count down
};
