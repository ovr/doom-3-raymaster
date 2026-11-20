// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "../../idlib/precompiled.h"

#include "../tr_local.h"
#include "../RenderProgSource.h"
#include "d3d12_HlslEmitter.h"
#include "d3d12_ShaderCompiler.h"

idCVar r_d3d12DumpHLSL( "r_d3d12DumpHLSL", "0", CVAR_RENDERER | CVAR_BOOL,
	"1 = Direct3D 12 writes every HLSL text it compiles (or finds in the cache) to fs_savepath/renderprogs/hlsl/<name>.<vs|ps>.hlsl" );

namespace {

constexpr int DXBC_HEADER = 32;		// 'DXBC', the checksum, 1, the total size, the chunk count

idStr BlobText( ID3DBlob * blob ) {
	if ( blob == nullptr || blob->GetBufferSize() == 0 ) {
		return idStr();
	}
	// idStr::Append stops at a NUL but counts the length given: the terminator of the blob stays out
	const char * chars = static_cast<const char *>( blob->GetBufferPointer() );
	idStr text;
	text.Append( chars, static_cast<int>( strnlen( chars, blob->GetBufferSize() ) ) );
	text.StripTrailingWhitespace();
	return text;
}

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

// A file another FXC or a crash cut short is not used.
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

uint64 idD3D12ShaderCompiler::Hash( std::string_view hlsl, shaderStageType_t stage ) const {
	const bool vertex = ( stage == shaderStageType_t::VERTEX );
	const uint64 key[] = { static_cast<uint64>( hlsl.size() ), FLAGS, library.compilerVersion, static_cast<uint64>( idD3D12HlslEmitter::VERSION ) };
	const char * profile = vertex ? "vs_5_1 vsMain" : "ps_5_1 psMain";
	MD5_CTX context;
	MD5_Init( &context );
	MD5_Update( &context, reinterpret_cast<const unsigned char *>( hlsl.data() ), hlsl.size() );
	MD5_Update( &context, reinterpret_cast<const unsigned char *>( profile ), strlen( profile ) );
	MD5_Update( &context, reinterpret_cast<const unsigned char *>( key ), sizeof( key ) );
	unsigned char digest[16];
	MD5_Final( &context, digest );
	uint64 halves[2];
	memcpy( halves, digest, sizeof( halves ) );
	return halves[0] ^ halves[1];
}

std::expected<d3d12Bytecode_t, idStr> idD3D12ShaderCompiler::Compile( std::string_view hlsl, shaderStageType_t stage, const char * name ) {
	if ( r_d3d12DumpHLSL.GetBool() ) {
		fileSystem->WriteFile( va( "renderprogs/hlsl/%s.%s.hlsl", FileName( name ).c_str(), StageSuffix( stage ) ), hlsl.data(), static_cast<int>( hlsl.size() ) );
	}
	const uint64 hash = Hash( hlsl, stage );
	if ( std::optional<d3d12Bytecode_t> cached = cache.Find( name, stage, hash ) ) {
		hits++;
		return std::move( *cached );
	}

	const bool vertex = ( stage == shaderStageType_t::VERTEX );
	idComPtr<ID3DBlob> code;
	idComPtr<ID3DBlob> messages;
	const HRESULT hr = library.compile( hlsl.data(), hlsl.size(), name, nullptr, nullptr, vertex ? "vsMain" : "psMain",
		vertex ? "vs_5_1" : "ps_5_1", FLAGS, 0, &code, &messages );
	const idStr text = BlobText( messages.Get() );
	if ( FAILED( hr ) || !code ) {
		return std::unexpected( idStr( va( "%s: %s", name, text.Length() > 0 ? text.c_str() : D3D12_ErrorText( hr ).c_str() ) ) );
	}
	if ( text.Length() > 0 ) {
		common->DPrintf( "d3d12: %s: %s\n", name, text.c_str() );
	}
	compiled++;
	const byte * bytes = static_cast<const byte *>( code->GetBufferPointer() );
	d3d12Bytecode_t result( bytes, bytes + code->GetBufferSize() );
	cache.Store( name, stage, hash, result );
	return result;
}

d3d12StageReflection_t idD3D12ShaderCompiler::Reflect( std::span<const byte> code, shaderStageType_t stage ) const {
	idComPtr<ID3D12ShaderReflection> reflection;
	const HRESULT hr = library.reflect( code.data(), code.size(), __uuidof( ID3D12ShaderReflection ), reinterpret_cast<void **>( reflection.GetAddressOf() ) );
	if ( FAILED( hr ) || !reflection ) {
		common->Warning( "d3d12: D3DReflect: %s", D3D12_ErrorText( hr ).c_str() );
		return {};
	}
	return D3D12_ReflectStage( *reflection.Get(), stage );
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
		if ( FAILED( reflection.GetResourceBindingDesc( i, &bind ) ) || bind.Space != 0 ) {
			continue;
		}
		if ( bind.Type == D3D_SIT_TEXTURE && bind.BindPoint < 16 ) {
			result.samplerMask = static_cast<uint16>( result.samplerMask | ( 1u << bind.BindPoint ) );
			if ( bind.Dimension == D3D_SRV_DIMENSION_TEXTURECUBE ) {
				result.cubeMask = static_cast<uint16>( result.cubeMask | ( 1u << bind.BindPoint ) );
			}
		} else if ( bind.Type == D3D_SIT_CBUFFER && bind.BindPoint == 2 ) {
			result.usesJoints = true;
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
