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

#pragma hdrstop
#include "../../idlib/precompiled.h"

#include "../tr_local.h"
#include "d3d12_Programs.h"

#include <algorithm>
#include <functional>

namespace {

// "gui.vfp" → "gui": the compiler's name for messages and cache files.
[[nodiscard]] idStr BaseName( const char * name ) {
	idStr base = name;
	base.StripFileExtension();
	return base;
}

[[nodiscard]] std::string_view View( const idStr & text ) {
	return std::string_view( text.c_str(), static_cast<size_t>( text.Length() ) );
}

}	// namespace

std::expected<d3d12ShaderStage_t, idStr> D3D12_BuildStage( idD3D12ShaderCompiler & compiler, shaderStageType_t type, const char * name,
	const renderProgSource_t & source ) {
	d3d12ShaderStage_t stage;
	stage.type = type;
	stage.name = name;
	auto emitted = idD3D12HlslEmitter::Emit( type, source, R_RenderProgPath( name, type ) );
	if ( !emitted ) {
		return std::unexpected( emitted.error() );
	}
	stage.emitted = std::move( *emitted );
	const idStr hlsl = idD3D12HlslEmitter::WithEntry( type, stage.emitted, stage.emitted.varyings );
	auto code = compiler.Compile( View( hlsl ), type, BaseName( name ) );
	if ( !code ) {
		return std::unexpected( code.error() );
	}
	stage.code = std::move( *code );
	stage.reflection = compiler.Reflect( stage.code, type );
	return stage;
}

// GL reflects the linked program: the masks of the PS, the inputs and the joints of the VS of the pair.
std::expected<d3d12Program_t, idStr> D3D12_LinkProgram( idD3D12ShaderCompiler & compiler, const d3d12ShaderStage_t & vertex,
	const d3d12ShaderStage_t * fragment ) {
	d3d12Program_t program;
	program.vertexInfo = vertex.emitted.info;
	d3d12StageReflection_t vertexReflection = vertex.reflection;
	if ( fragment == nullptr || fragment->emitted.varyings == vertex.emitted.varyings ) {
		program.vertexCode = vertex.code;
	} else {
		const idStr hlsl = idD3D12HlslEmitter::WithEntry( shaderStageType_t::VERTEX, vertex.emitted, fragment->emitted.varyings );
		const idStr name = BaseName( vertex.name ) + "." + BaseName( fragment->name );
		auto code = compiler.Compile( View( hlsl ), shaderStageType_t::VERTEX, name );
		if ( !code ) {
			return std::unexpected( code.error() );
		}
		program.vertexCode = std::move( *code );
		vertexReflection = compiler.Reflect( program.vertexCode, shaderStageType_t::VERTEX );
	}
	if ( fragment != nullptr ) {
		program.fragmentCode = fragment->code;
		program.fragmentInfo = fragment->emitted.info;
		program.info.samplerMask = fragment->reflection.samplerMask;
		program.info.cubeMask = fragment->reflection.cubeMask;
	}
	program.info.attribMask = vertexReflection.attribMask;
	program.info.usesJoints = vertexReflection.usesJoints;
	program.journalName = vertex.name + "|" + ( ( fragment != nullptr ) ? fragment->name.c_str() : "-" );
	return program;
}

// The source is read only for a stage GL's probe finds; D3D12 compiles the loose sources GL had converted into the packs.
shaderHandle_t idD3D12ShaderLibrary::CreateStage( shaderStageType_t type, const char * name, shaderStageInfo_t & info ) {
	info.numParms = 0;
	if ( !R_RenderProgStageExists( name, type ) ) {
		d3d12ShaderStage_t missing;
		missing.type = type;
		missing.name = name;
		missing.missing = true;
		return stages.Add( std::move( missing ) );
	}
	const std::optional<renderProgSource_t> source = R_LoadRenderProgSource( name, type );
	if ( !source ) {
		common->Warning( "d3d12: %s: no source to compile", R_RenderProgPath( name, type ).c_str() );
		return shaderHandle_t::NONE;
	}
	auto stage = D3D12_BuildStage( compiler, type, name, *source );
	if ( !stage ) {
		common->Warning( "d3d12: %s", stage.error().c_str() );
		return shaderHandle_t::NONE;
	}
	info = stage->emitted.info;
	return stages.Add( std::move( *stage ) );
}

programHandle_t idD3D12ShaderLibrary::CreateProgram( shaderHandle_t vertex, shaderHandle_t fragment, const char * name, programInfo_t & info ) {
	info = {};
	const d3d12ShaderStage_t * vertexStage = stages.Find( vertex );
	const d3d12ShaderStage_t * fragmentStage = stages.Find( fragment );
	if ( ( vertexStage != nullptr && vertexStage->missing ) || ( fragmentStage != nullptr && fragmentStage->missing ) ) {
		// GL's link of a stage without a shader fails: the Error ends the map load, vdemo loads it again
		idLib::Error( "While linking GLSL program %s with vertexShader %s and fragmentShader %s\n", name,
			( vertexStage != nullptr ) ? vertexStage->name.c_str() : "<Invalid>", ( fragmentStage != nullptr ) ? fragmentStage->name.c_str() : "<Invalid>" );
	}
	if ( vertexStage == nullptr ) {
		common->Warning( "d3d12: program %s has no vertex stage", name );
		return programHandle_t::NONE;
	}
	auto program = D3D12_LinkProgram( compiler, *vertexStage, fragmentStage );
	if ( !program ) {
		common->Warning( "d3d12: program %s: %s", name, program.error().c_str() );
		return programHandle_t::NONE;
	}
	const programInfo_t linked = program->info;
	const programHandle_t handle = AddProgram( std::move( *program ) );
	if ( handle != programHandle_t::NONE ) {
		info = linked;
	}
	return handle;
}

programHandle_t idD3D12ShaderLibrary::AddProgram( d3d12Program_t && program ) {
	program.dense = NewDense();
	const uint16 dense = program.dense;
	const programHandle_t handle = programs.Add( std::move( program ) );
	if ( handle == programHandle_t::NONE ) {
		FreeDense( dense );
	}
	return handle;
}

void idD3D12ShaderLibrary::DestroyStage( shaderHandle_t stage ) {
	if ( stages.Find( stage ) != nullptr ) {
		static_cast<void>( stages.Remove( stage ) );
	}
}

void idD3D12ShaderLibrary::DestroyProgram( programHandle_t handle ) {
	if ( programs.Find( handle ) == nullptr ) {
		return;
	}
	d3d12Program_t program = programs.Remove( handle );
	for ( d3d12Pipeline_t & pipeline : program.pipelines ) {
		graveyard.Bury( std::move( pipeline.pso ) );
	}
	FreeDense( program.dense );
}

void idD3D12ShaderLibrary::DestroyAll() {
	programs.ForEach( [this]( programHandle_t, d3d12Program_t & program ) {
		for ( d3d12Pipeline_t & pipeline : program.pipelines ) {
			graveyard.Bury( std::move( pipeline.pso ) );
		}
	} );
	programs.Clear();
	stages.Clear();
	freeDense.clear();
	nextDense = 0;
}

uint16 idD3D12ShaderLibrary::NewDense() {
	if ( freeDense.empty() ) {
		return nextDense++;
	}
	std::ranges::pop_heap( freeDense, std::greater<>() );
	const uint16 dense = freeDense.back();
	freeDense.pop_back();
	return dense;
}

void idD3D12ShaderLibrary::FreeDense( uint16 dense ) {
	freeDense.push_back( dense );
	std::ranges::push_heap( freeDense, std::greater<>() );
}

// Through the cache like a map load, so a later load finds what this compiled.
int idD3D12ShaderLibrary::CompileAll( bool verbose ) {
	int sources = 0;
	idStrList failures;
	for ( const shaderStageType_t type : { shaderStageType_t::VERTEX, shaderStageType_t::FRAGMENT } ) {
		idFileList * files = fileSystem->ListFiles( "renderprogs", ( type == shaderStageType_t::VERTEX ) ? ".vertex" : ".pixel", true );
		for ( int i = 0; i < files->GetNumFiles(); i++ ) {
			const char * file = files->GetFile( i );
			sources++;
			const std::optional<renderProgSource_t> source = R_LoadRenderProgSource( file, type );
			if ( !source ) {
				common->Printf( "d3d12_compileAll: %s: unreadable, or a uniform no parm names\n", file );
				failures.Append( file );
				continue;
			}
			auto stage = D3D12_BuildStage( compiler, type, file, *source );
			if ( !stage ) {
				common->Printf( "d3d12_compileAll: %s\n", stage.error().c_str() );
				failures.Append( file );
			} else if ( verbose ) {
				common->Printf( "d3d12_compileAll: %s: %d parms, varyings 0x%03x, attribs 0x%04x, samplers 0x%04x\n", file, stage->emitted.info.numParms,
					stage->emitted.varyings, stage->emitted.attribs, stage->reflection.samplerMask );
			}
		}
		fileSystem->FreeFileList( files );
	}
	idStr list;
	for ( int i = 0; i < failures.Num(); i++ ) {
		list += ( i == 0 ) ? ": " : ", ";
		list += failures[i];
	}
	common->Printf( "d3d12_compileAll: %d sources, %d errors%s\n", sources, failures.Num(), list.c_str() );
	return failures.Num();
}
