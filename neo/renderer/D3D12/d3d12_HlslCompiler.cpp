// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "../../idlib/precompiled.h"

#include "../tr_local.h"
#include "../RenderProgTable.h"
#include "d3d12_HlslCompiler.h"

namespace {

constexpr UINT VERTEX_PARMS = 0;	// the root signature's CBVs
constexpr UINT FRAGMENT_PARMS = 1;
constexpr UINT JOINTS = 2;

[[nodiscard]] const char * Entry( shaderStageType_t stage ) {
	return ( stage == shaderStageType_t::VERTEX ) ? "VSMain" : "PSMain";
}

[[nodiscard]] const char * Profile( shaderStageType_t stage ) {
	return ( stage == shaderStageType_t::VERTEX ) ? "vs_6_6" : "ps_6_6";
}

[[nodiscard]] std::string_view View( const idStr & text ) {
	return std::string_view( text.c_str(), static_cast<size_t>( text.Length() ) );
}

// The name as a whole identifier before a '(': a function, not a word of another name.
[[nodiscard]] bool NamesFunction( std::string_view text, std::string_view name ) {
	const auto identifier = []( char c ) { return isalnum( static_cast<unsigned char>( c ) ) != 0 || c == '_'; };
	for ( size_t at = text.find( name ); at != std::string_view::npos; at = text.find( name, at + 1 ) ) {
		size_t next = at + name.size();
		while ( next < text.size() && ( text[next] == ' ' || text[next] == '\t' ) ) {
			next++;
		}
		if ( ( at == 0 || !identifier( text[at - 1] ) ) && next < text.size() && text[next] == '(' ) {
			return true;
		}
	}
	return false;
}

// b3 as bindless.hlsli declares it: uints in d3d12DrawResources_t's order, so a root constant lands on its member.
[[nodiscard]] std::optional<idStr> DrawResourcesMismatch( ID3D12ShaderReflection & reflection, const char * block ) {
	ID3D12ShaderReflectionConstantBuffer * buffer = reflection.GetConstantBufferByName( block );
	D3D12_SHADER_BUFFER_DESC desc = {};
	if ( buffer == nullptr || FAILED( buffer->GetDesc( &desc ) ) || desc.Variables != d3d12DrawResources_t::COUNT ) {
		return idStr( va( "cbuffer %s at b3 is not the one of include/bindless.hlsli", block ) );
	}
	for ( UINT v = 0; v < desc.Variables; v++ ) {
		ID3D12ShaderReflectionVariable * variable = buffer->GetVariableByIndex( v );
		D3D12_SHADER_VARIABLE_DESC variableDesc = {};
		D3D12_SHADER_TYPE_DESC type = {};
		if ( FAILED( variable->GetDesc( &variableDesc ) ) || FAILED( variable->GetType()->GetDesc( &type ) ) ) {
			return idStr( va( "no reflection of member %u of cbuffer %s", v, block ) );
		}
		const int slot = static_cast<int>( v % MAX_TEXTURE_SLOTS );
		const char * expected = ( v < d3d12DrawResources_t::CUBES ) ? va( "texture2D%d", slot )
			: ( ( v < d3d12DrawResources_t::SAMPLERS ) ? va( "textureCube%d", slot ) : va( "sampler%d", slot ) );
		if ( idStr::Cmp( variableDesc.Name, expected ) != 0 || variableDesc.StartOffset != v * sizeof( uint32 ) || type.Class != D3D_SVC_SCALAR
				|| type.Type != D3D_SVT_UINT ) {
			return idStr( va( "%s in cbuffer %s at b3: include/bindless.hlsli has %s there", variableDesc.Name, block, expected ) );
		}
	}
	return std::nullopt;
}

// The members of the stage's cbuffer in offset order; every other binding must be one the root signature has.
[[nodiscard]] std::expected<shaderStageInfo_t, idStr> ReadParms( ID3D12ShaderReflection & reflection, shaderStageType_t stage, const char * name,
	hlslParms_t parms ) {
	const bool vertex = ( stage == shaderStageType_t::VERTEX );
	const UINT own = vertex ? VERTEX_PARMS : FRAGMENT_PARMS;
	shaderStageInfo_t info = {};
	D3D12_SHADER_DESC desc = {};
	if ( FAILED( reflection.GetDesc( &desc ) ) ) {
		return std::unexpected( idStr( va( "%s: no reflection", name ) ) );
	}
	for ( UINT i = 0; i < desc.BoundResources; i++ ) {
		D3D12_SHADER_INPUT_BIND_DESC bind = {};
		if ( FAILED( reflection.GetResourceBindingDesc( i, &bind ) ) ) {
			continue;
		}
		const bool cbuffer = ( bind.Type == D3D_SIT_CBUFFER );
		const bool resources = cbuffer && bind.BindPoint == d3d12DrawResources_t::REGISTER;
		const bool fits = ( bind.Space == 0 ) && cbuffer && ( bind.BindPoint == own || resources || ( vertex && bind.BindPoint == JOINTS ) );
		if ( !fits ) {
			return std::unexpected( idStr( va( "%s: %s binds %s at register %u space %u: the %s stage has b%u%s and b3, and takes textures and "
				"samplers through TEXTURE2D, TEXTURECUBE and SAMPLER (include/bindless.hlsli)", name, vertex ? "VSMain" : "PSMain", bind.Name,
				bind.BindPoint, bind.Space, vertex ? "vertex" : "pixel", own, vertex ? ", b2" : "" ) ) );
		}
		if ( resources ) {
			if ( const std::optional<idStr> mismatch = DrawResourcesMismatch( reflection, bind.Name ) ) {
				return std::unexpected( idStr( va( "%s: %s", name, mismatch->c_str() ) ) );
			}
			continue;
		}
		if ( !cbuffer || bind.BindPoint != own || parms != hlslParms_t::RENDER_PARMS ) {
			continue;
		}
		ID3D12ShaderReflectionConstantBuffer * block = reflection.GetConstantBufferByName( bind.Name );
		D3D12_SHADER_BUFFER_DESC blockDesc = {};
		if ( block == nullptr || FAILED( block->GetDesc( &blockDesc ) ) ) {
			return std::unexpected( idStr( va( "%s: no reflection of cbuffer %s", name, bind.Name ) ) );
		}
		for ( UINT v = 0; v < blockDesc.Variables; v++ ) {
			ID3D12ShaderReflectionVariable * variable = block->GetVariableByIndex( v );
			D3D12_SHADER_VARIABLE_DESC variableDesc = {};
			D3D12_SHADER_TYPE_DESC type = {};
			if ( FAILED( variable->GetDesc( &variableDesc ) ) || FAILED( variable->GetType()->GetDesc( &type ) ) ) {
				return std::unexpected( idStr( va( "%s: no reflection of member %u of cbuffer %s", name, v, bind.Name ) ) );
			}
			const std::optional<renderParm_t> parm = R_FindRenderParm( variableDesc.Name );
			const bool float4 = ( type.Class == D3D_SVC_VECTOR && type.Type == D3D_SVT_FLOAT && type.Columns == 4 && type.Elements == 0 );
			if ( !parm || !float4 || variableDesc.StartOffset != v * sizeof( idVec4 ) || info.numParms == MAX_RENDER_PARMS ) {
				return std::unexpected( idStr( va( "%s: %s in cbuffer %s: every member must be a float4 named like a renderParm_t", name,
					variableDesc.Name, bind.Name ) ) );
			}
			info.parms[info.numParms++] = static_cast<uint8>( *parm );
		}
	}
	return info;
}

}	// namespace

std::optional<d3d12HlslSource_t> D3D12_LoadHlslFile( const char * path, const char * name, const d3d12ShaderReader_t & reader ) {
	std::optional<idStr> text = reader( path );
	if ( !text ) {
		return std::nullopt;
	}
	d3d12HlslSource_t source;
	source.name = name;
	source.path = path;
	source.text = std::move( *text );
	return source;
}

std::optional<d3d12HlslSource_t> D3D12_FindHlslSource( const char * stageName, const d3d12ShaderReader_t & reader ) {
	d3d12HlslSource_t source;
	source.name = stageName;
	source.name.StripFileExtension();
	source.path = va( "shaders/%s.hlsl", source.name.c_str() );
	if ( std::optional<idStr> text = reader( source.path ) ) {
		source.text = std::move( *text );
		return source;
	}
	constexpr const char * SKINNED = "_skinned";
	const int base = source.name.Length() - static_cast<int>( strlen( SKINNED ) );
	if ( base <= 0 || idStr::Icmp( source.name.c_str() + base, SKINNED ) != 0 ) {
		return std::nullopt;
	}
	source.path = va( "shaders/%s.hlsl", source.name.Left( base ).c_str() );
	if ( std::optional<idStr> text = reader( source.path ) ) {
		source.text = std::move( *text );
		source.defines.Append( "SKINNED=1" );
		return source;
	}
	return std::nullopt;
}

std::expected<std::vector<d3d12HlslEntry_t>, idStr> D3D12_HlslEntries( const idD3D12Library & library, const d3d12HlslSource_t & plain,
	const d3d12ShaderReader_t & reader ) {
	constexpr const char * SKINNED[] = { "SKINNED=1" };
	const auto text = D3D12_DxcPreprocess( library, View( plain.text ), plain.path, {}, reader );
	if ( !text ) {
		return std::unexpected( text.error() );
	}
	const auto skinned = D3D12_DxcPreprocess( library, View( plain.text ), plain.path, SKINNED, reader );
	if ( !skinned ) {
		return std::unexpected( skinned.error() );
	}
	std::vector<d3d12HlslEntry_t> entries;
	const auto add = [&entries]( const d3d12HlslSource_t & source, const idStr & preprocessed ) {
		for ( const shaderStageType_t stage : { shaderStageType_t::VERTEX, shaderStageType_t::FRAGMENT } ) {
			if ( NamesFunction( View( preprocessed ), Entry( stage ) ) ) {
				entries.push_back( { source, stage } );
			}
		}
	};
	add( plain, *text );
	if ( idStr::Cmp( *skinned, *text ) != 0 ) {
		d3d12HlslSource_t variant = plain;
		variant.name += "_skinned";
		variant.defines.Append( SKINNED[0] );
		add( variant, *skinned );
	}
	return entries;
}

d3d12ShaderReader_t D3D12_GameDataReader() {
	return []( const char * path ) -> std::optional<idStr> {
		void * buffer = nullptr;
		const int length = fileSystem->ReadFile( path, &buffer );
		if ( length < 0 || buffer == nullptr ) {
			return std::nullopt;
		}
		idStr text;
		text.Append( static_cast<const char *>( buffer ), length );
		fileSystem->FreeFile( buffer );
		return text;
	};
}

uint64 idD3D12HlslCompiler::Hash( std::string_view preprocessed, shaderStageType_t stage ) const {
	const uint64 key[] = { static_cast<uint64>( preprocessed.size() ), library.dxcVersion, static_cast<uint64>( VERSION ) };
	const idStr target = va( "%s %s", Profile( stage ), Entry( stage ) );
	MD5_CTX context;
	MD5_Init( &context );
	MD5_Update( &context, reinterpret_cast<const unsigned char *>( preprocessed.data() ), preprocessed.size() );
	MD5_Update( &context, reinterpret_cast<const unsigned char *>( target.c_str() ), static_cast<unsigned int>( target.Length() ) );
	MD5_Update( &context, reinterpret_cast<const unsigned char *>( key ), sizeof( key ) );
	unsigned char digest[16];
	MD5_Final( &context, digest );
	uint64 halves[2];
	memcpy( halves, digest, sizeof( halves ) );
	return halves[0] ^ halves[1];
}

std::expected<d3d12HlslStage_t, idStr> idD3D12HlslCompiler::Build( const d3d12HlslSource_t & source, shaderStageType_t stage, hlslParms_t parms ) {
	std::vector<const char *> defines;
	for ( int i = 0; i < source.defines.Num(); i++ ) {
		defines.push_back( source.defines[i].c_str() );
	}
	// the hash covers every include: the text is preprocessed first, and that text is what DXC compiles
	const auto preprocessed = D3D12_DxcPreprocess( library, View( source.text ), source.path, defines, reader );
	if ( !preprocessed ) {
		return std::unexpected( preprocessed.error() );
	}
	const uint64 hash = Hash( View( *preprocessed ), stage );
	d3d12HlslStage_t result;
	if ( std::optional<d3d12Bytecode_t> cached = cache.Find( source.name, stage, hash ); cached && D3D12_DxilSigned( *cached ) ) {
		hits++;
		result.code = std::move( *cached );
	} else {
		auto output = D3D12_DxcCompile( library, View( *preprocessed ), source.path, Entry( stage ), Profile( stage ) );
		if ( !output ) {
			return std::unexpected( output.error() );
		}
		// the pipeline library names a PSO by the container digests, and the runtime refuses unsigned DXIL
		if ( !D3D12_DxilSigned( output->code ) ) {
			return std::unexpected( idStr( va( "%s: dxil.dll did not sign the DXIL", source.name.c_str() ) ) );
		}
		if ( !output->warnings.IsEmpty() ) {
			common->DPrintf( "d3d12: %s: %s\n", source.name.c_str(), output->warnings.c_str() );
		}
		compiled++;
		result.code = std::move( output->code );
		cache.Store( source.name, stage, hash, result.code );
	}
	const auto reflection = D3D12_DxcReflect( library, result.code );
	if ( !reflection ) {
		return std::unexpected( idStr( va( "%s: %s", source.name.c_str(), reflection.error().c_str() ) ) );
	}
	auto info = ReadParms( *reflection->Get(), stage, source.name, parms );
	if ( !info ) {
		return std::unexpected( info.error() );
	}
	result.info = *info;
	result.reflection = D3D12_ReflectStage( *reflection->Get(), stage );
	return result;
}
