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

// A pipeline state stream subobject as the runtime parses it: the type, then the value, pointer aligned.
// The padding the alignment adds is what the stream wants (C4324).
#pragma warning( push )
#pragma warning( disable : 4324 )
template<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE TYPE, typename T>
struct alignas( void * ) d3d12Subobject_t {
	D3D12_PIPELINE_STATE_SUBOBJECT_TYPE	type = TYPE;
	T								value = {};
};
#pragma warning( pop )

// DEPTH_STENCIL1 carries DepthBoundsTestEnable, which the plain graphics description lacks.
struct d3d12PipelineStream_t {
	d3d12Subobject_t<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *>		root;
	d3d12Subobject_t<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE>					vs;
	d3d12Subobject_t<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE>					ps;
	d3d12Subobject_t<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC>					blend;
	d3d12Subobject_t<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT>							sampleMask;
	d3d12Subobject_t<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC>			raster;
	d3d12Subobject_t<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, D3D12_DEPTH_STENCIL_DESC1>	depthStencil;
	d3d12Subobject_t<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT, D3D12_INPUT_LAYOUT_DESC>		inputLayout;
	d3d12Subobject_t<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE>	topology;
	d3d12Subobject_t<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY>	targets;
	d3d12Subobject_t<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT>			depthFormat;
	d3d12Subobject_t<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC>				sample;
};

// "doom3 pso gui.vfp|gui.vfp": the program as PIX and the debug layer name it.
[[nodiscard]] std::wstring PipelineName( const d3d12Program_t & program ) {
	std::wstring name = L"doom3 pso ";
	for ( const char c : std::string_view( program.journalName.c_str(), static_cast<size_t>( program.journalName.Length() ) ) ) {
		name.push_back( static_cast<wchar_t>( static_cast<unsigned char>( c ) ) );
	}
	return name;
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

uint8 idD3D12DepthBiasTable::Slot( float factor, float units ) {
	if ( factor != lastFactor || units != lastUnits ) {
		const d3d12DepthBias_t wanted = D3D12_DepthBiasFor( factor, units );
		lastFactor = factor;
		lastUnits = units;
		lastSlot = 0;
		lastNearest = false;
		if ( wanted != d3d12DepthBias_t() ) {
			const std::span used( biases.data(), static_cast<size_t>( count ) );
			if ( const auto found = std::ranges::find( used, wanted ); found != used.end() ) {
				lastSlot = static_cast<uint8>( found - used.begin() + 1 );
			} else if ( count < static_cast<int>( biases.size() ) ) {
				biases[count++] = wanted;
				lastSlot = static_cast<uint8>( count );
			} else {
				if ( !reportedFull ) {
					common->Warning( "d3d12: all %d depth bias slots are in use, new biases draw with the nearest one (see r_d3d12Stats)", count );
					reportedFull = true;
				}
				lastSlot = NearestSlot( wanted );
				lastNearest = true;
			}
		}
	}
	nearest += lastNearest ? 1 : 0;
	return lastSlot;
}

// Factor and units weigh the same: the depth a factor adds depends on the surface's slope, unknown here.
// A tie goes to the lower slot, so the choice repeats from run to run.
uint8 idD3D12DepthBiasTable::NearestSlot( const d3d12DepthBias_t & wanted ) const {
	const auto distance = [&wanted]( const d3d12DepthBias_t & bias ) {
		const double units = static_cast<double>( bias.bias ) - static_cast<double>( wanted.bias );
		const double factor = static_cast<double>( bias.slope ) - static_cast<double>( wanted.slope );
		return units * units + factor * factor;
	};
	uint8 best = 0;
	double bestDistance = distance( d3d12DepthBias_t() );
	for ( int slot = 1; slot <= count; slot++ ) {
		if ( const double candidate = distance( biases[slot - 1] ); candidate < bestDistance ) {
			best = static_cast<uint8>( slot );
			bestDistance = candidate;
		}
	}
	return best;
}

d3d12DepthBias_t idD3D12DepthBiasTable::operator[]( uint8 slot ) const {
	assert( slot <= count );
	return ( slot == 0 || slot > count ) ? d3d12DepthBias_t() : biases[slot - 1];
}

idD3D12PipelineFactory::idD3D12PipelineFactory( const idD3D12Device & owner, const idD3D12RootSignature & signature ) :
	device( owner ),
	root( signature ) {
}

ID3D12PipelineState * idD3D12PipelineFactory::Get( d3d12Program_t & program, const pipelineKey_t & key ) {
	for ( const d3d12Pipeline_t & pipeline : program.pipelines ) {
		if ( pipeline.key == key ) {
			return pipeline.pso.Get();
		}
	}
	idComPtr<ID3D12PipelineState> pso = Create( program, key );
	if ( !pso ) {
		return nullptr;
	}
	created++;
	total++;
	program.pipelines.push_back( { key, std::move( pso ) } );
	return program.pipelines.back().pso.Get();
}

idComPtr<ID3D12PipelineState> idD3D12PipelineFactory::Create( const d3d12Program_t & program, const pipelineKey_t & key ) const {
	const d3d12PipelineDesc_t desc = D3D12_PipelineDescFor( key, program.info.attribMask, bias[key.depthBias], device.Features().depthBounds );
	d3d12PipelineStream_t stream;
	stream.root.value = root.Get();
	stream.vs.value = { program.vertexCode.data(), program.vertexCode.size() };
	if ( !program.fragmentCode.empty() ) {
		stream.ps.value = { program.fragmentCode.data(), program.fragmentCode.size() };
	}
	stream.blend.value = desc.blend;
	stream.sampleMask.value = UINT_MAX;
	stream.raster.value = desc.raster;
	stream.depthStencil.value = desc.depthStencil;
	stream.inputLayout.value = { desc.inputs.data(), desc.numInputs };
	stream.topology.value = desc.topology;
	stream.targets.value.NumRenderTargets = 1;
	stream.targets.value.RTFormats[0] = desc.colorFormat;
	stream.depthFormat.value = d3d12TargetId_t::DEPTH;
	stream.sample.value = desc.sample;
	const D3D12_PIPELINE_STATE_STREAM_DESC streamDesc = { sizeof( stream ), &stream };
	idComPtr<ID3D12PipelineState> pso;
	if ( device.Check( device.Get().CreatePipelineState( &streamDesc, IID_PPV_ARGS( &pso ) ), "CreatePipelineState" ) ) {
		pso->SetName( PipelineName( program ).c_str() );
	}
	return pso;
}
