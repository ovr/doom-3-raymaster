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
#include "d3d12_Pipelines.h"

namespace {

D3D12_ROOT_PARAMETER1 RootCbv( UINT shaderRegister, D3D12_SHADER_VISIBILITY visibility ) {
	D3D12_ROOT_PARAMETER1 parameter = {};
	parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	parameter.Descriptor.ShaderRegister = shaderRegister;
	// written before the draw is recorded and kept until its fence is waited for
	parameter.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
	parameter.ShaderVisibility = visibility;
	return parameter;
}

D3D12_ROOT_PARAMETER1 Table( const D3D12_DESCRIPTOR_RANGE1 & range ) {
	D3D12_ROOT_PARAMETER1 parameter = {};
	parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	parameter.DescriptorTable.NumDescriptorRanges = 1;
	parameter.DescriptorTable.pDescriptorRanges = &range;
	parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	return parameter;
}

}	// namespace

idD3D12RootSignature::idD3D12RootSignature( const idD3D12Device & device, const idD3D12Library & library ) {
	// an upload or a copy may change a texture while a recorded table names it (rule R4)
	D3D12_DESCRIPTOR_RANGE1 textures = {};
	textures.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	textures.NumDescriptors = MAX_TEXTURE_SLOTS;
	textures.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
	D3D12_DESCRIPTOR_RANGE1 samplers = {};
	samplers.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
	samplers.NumDescriptors = MAX_TEXTURE_SLOTS;

	std::array<D3D12_ROOT_PARAMETER1, D3D12_Root( d3d12RootSlot_t::COUNT )> parameters;
	parameters[D3D12_Root( d3d12RootSlot_t::VERTEX_PARMS )] = RootCbv( 0, D3D12_SHADER_VISIBILITY_VERTEX );
	parameters[D3D12_Root( d3d12RootSlot_t::FRAGMENT_PARMS )] = RootCbv( 1, D3D12_SHADER_VISIBILITY_PIXEL );
	parameters[D3D12_Root( d3d12RootSlot_t::JOINTS )] = RootCbv( 2, D3D12_SHADER_VISIBILITY_VERTEX );
	parameters[D3D12_Root( d3d12RootSlot_t::TEXTURES )] = Table( textures );
	parameters[D3D12_Root( d3d12RootSlot_t::SAMPLERS )] = Table( samplers );

	D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
	desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
	desc.Desc_1_1.NumParameters = static_cast<UINT>( parameters.size() );
	desc.Desc_1_1.pParameters = parameters.data();
	desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
		| D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	idComPtr<ID3DBlob> blob;
	idComPtr<ID3DBlob> error;
	const HRESULT hr = library.serializeRootSignature( &desc, &blob, &error );
	if ( FAILED( hr ) ) {
		common->FatalError( "d3d12: D3D12SerializeVersionedRootSignature failed: %s %s", D3D12_ErrorText( hr ).c_str(),
			error ? static_cast<const char *>( error->GetBufferPointer() ) : "" );
	}
	if ( device.Check( device.Get().CreateRootSignature( 0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS( &signature ) ),
			"CreateRootSignature" ) ) {
		signature->SetName( L"doom3 root signature" );
	}
}
