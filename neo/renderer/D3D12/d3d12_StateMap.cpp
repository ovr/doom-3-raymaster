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

#include <bit>

#include "../tr_local.h"
#include "d3d12_StateMap.h"

namespace {

constexpr int MAX_ANISOTROPY = 16;

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
