// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "../../idlib/precompiled.h"

#include "../tr_local.h"
#include "../RenderProgSource.h"
#include "d3d12_ShaderCompiler.h"

namespace {

constexpr int DXBC_HEADER = 32;		// 'DXBC', the checksum, 1, the total size, the chunk count

[[nodiscard]] const char * StageSuffix( shaderStageType_t stage ) {
	return ( stage == shaderStageType_t::VERTEX ) ? "vs" : "ps";
}

// A name as one file name: "d3d12 present blit" → "d3d12_present_blit".
[[nodiscard]] idStr FileName( const char * name ) {
	idStr file = name;
	for ( int i = 0; i < file.Length(); i++ ) {
		const char c = file[i];
		if ( !idStr::CharIsAlpha( c ) && !idStr::CharIsNumeric( c ) && c != '_' && c != '.' && c != '-' ) {
			file[i] = '_';
		}
	}
	return file;
}

// A file that another run or a crash cut short is not used.
[[nodiscard]] bool IsDxbc( std::span<const byte> code ) {
	if ( code.size() < DXBC_HEADER || memcmp( code.data(), "DXBC", 4 ) != 0 ) {
		return false;
	}
	uint32 size = 0;
	memcpy( &size, code.data() + 24, sizeof( size ) );
	return size == code.size();
}

}	// namespace

// DXIL comes in the same container as DXBC.
idStr idD3D12BlobCache::Path( const char * name, shaderStageType_t stage, uint64 hash ) const {
	return va( "%s/%s.%s.%016llx.%s", directory, FileName( name ).c_str(), StageSuffix( stage ), static_cast<unsigned long long>( hash ), extension );
}

std::optional<d3d12Bytecode_t> idD3D12BlobCache::Find( const char * name, shaderStageType_t stage, uint64 hash ) const {
	if ( !enabled ) {
		return std::nullopt;
	}
	const idStr path = Path( name, stage, hash );
	void * buffer = nullptr;
	const int length = fileSystem->ReadFile( path, &buffer );
	if ( length <= 0 || buffer == nullptr ) {
		return std::nullopt;
	}
	const byte * bytes = static_cast<const byte *>( buffer );
	d3d12Bytecode_t code( bytes, bytes + length );
	fileSystem->FreeFile( buffer );
	if ( !IsDxbc( code ) ) {
		common->Warning( "d3d12: %s is no shader container; compiling again", path.c_str() );
		return std::nullopt;
	}
	return code;
}

void idD3D12BlobCache::Store( const char * name, shaderStageType_t stage, uint64 hash, std::span<const byte> code ) const {
	if ( enabled ) {
		fileSystem->WriteFile( Path( name, stage, hash ), code.data(), static_cast<int>( code.size() ) );
	}
}

namespace {

[[nodiscard]] d3d12SignatureElement_t SignatureElement( const D3D12_SIGNATURE_PARAMETER_DESC & parameter ) {
	d3d12SignatureElement_t element;
	element.semantic = parameter.SemanticName;
	element.semantic.ToUpper();
	element.index = parameter.SemanticIndex;
	element.reg = parameter.Register;
	element.mask = parameter.Mask;
	element.systemValue = parameter.SystemValueType;
	return element;
}

// The members of b3 the code reads: texture2D<n> and textureCube<n> are GL's active sampler uniforms.
void ReadDrawResources( ID3D12ShaderReflection & reflection, const char * name, d3d12StageReflection_t & result ) {
	ID3D12ShaderReflectionConstantBuffer * block = reflection.GetConstantBufferByName( name );
	D3D12_SHADER_BUFFER_DESC desc = {};
	if ( block == nullptr || FAILED( block->GetDesc( &desc ) ) ) {
		return;
	}
	for ( UINT i = 0; i < desc.Variables; i++ ) {
		D3D12_SHADER_VARIABLE_DESC variable = {};
		if ( FAILED( block->GetVariableByIndex( i )->GetDesc( &variable ) ) || ( variable.uFlags & D3D_SVF_USED ) == 0 ) {
			continue;
		}
		const uint32 index = static_cast<uint32>( variable.StartOffset / sizeof( uint32 ) );
		if ( index < d3d12DrawResources_t::SAMPLERS ) {
			const int slot = static_cast<int>( index % MAX_TEXTURE_SLOTS );
			result.samplerMask = static_cast<uint16>( result.samplerMask | ( 1u << slot ) );
			if ( index >= d3d12DrawResources_t::CUBES ) {
				result.cubeMask = static_cast<uint16>( result.cubeMask | ( 1u << slot ) );
			}
		}
	}
}

}	// namespace

// The rows of the input signature count whether or not the code reads them: GL's attribMask is a subset.
d3d12StageReflection_t D3D12_ReflectStage( ID3D12ShaderReflection & reflection, shaderStageType_t stage ) {
	d3d12StageReflection_t result;
	D3D12_SHADER_DESC desc = {};
	if ( FAILED( reflection.GetDesc( &desc ) ) ) {
		return result;
	}
	for ( UINT i = 0; i < desc.BoundResources; i++ ) {
		D3D12_SHADER_INPUT_BIND_DESC bind = {};
		if ( FAILED( reflection.GetResourceBindingDesc( i, &bind ) ) || bind.Space != 0 || bind.Type != D3D_SIT_CBUFFER ) {
			continue;
		}
		if ( bind.BindPoint == 2 ) {
			result.usesJoints = true;
		} else if ( bind.BindPoint == d3d12DrawResources_t::REGISTER ) {
			ReadDrawResources( reflection, bind.Name, result );
		}
	}
	const bool vertex = ( stage == shaderStageType_t::VERTEX );
	for ( UINT i = 0; i < ( vertex ? desc.OutputParameters : desc.InputParameters ); i++ ) {
		D3D12_SIGNATURE_PARAMETER_DESC parameter = {};
		if ( SUCCEEDED( vertex ? reflection.GetOutputParameterDesc( i, &parameter ) : reflection.GetInputParameterDesc( i, &parameter ) ) ) {
			( vertex ? result.outputs : result.inputs ).push_back( SignatureElement( parameter ) );
		}
	}
	if ( !vertex ) {
		return result;
	}
	for ( UINT i = 0; i < desc.InputParameters; i++ ) {
		D3D12_SIGNATURE_PARAMETER_DESC parameter = {};
		if ( FAILED( reflection.GetInputParameterDesc( i, &parameter ) ) ) {
			continue;
		}
		const d3d12SignatureElement_t element = SignatureElement( parameter );
		int attrib = R_VertexAttribIndex( va( "%s%u", element.semantic.c_str(), element.index ) );
		if ( attrib < 0 && element.index == 0 ) {
			attrib = R_VertexAttribIndex( element.semantic.c_str() );
		}
		if ( attrib >= 0 ) {
			result.attribMask = static_cast<uint16>( result.attribMask | ( 1 << attrib ) );
		}
	}
	return result;
}
