// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "../../idlib/precompiled.h"

#include "../tr_local.h"
#include "../RenderProgTable.h"
#include "d3d12_Programs.h"

#include <algorithm>
#include <functional>
#include <iterator>

namespace {

// D3D12 links the stages by register: what the PS reads (but the system values) the VS writes in the same place.
[[nodiscard]] std::optional<idStr> SignatureMismatch( const d3d12StageReflection_t & vertex, const d3d12StageReflection_t & fragment ) {
	for ( const d3d12SignatureElement_t & input : fragment.inputs ) {
		if ( input.systemValue != D3D_NAME_UNDEFINED ) {
			continue;
		}
		const auto output = std::ranges::find_if( vertex.outputs, [&input]( const d3d12SignatureElement_t & element ) {
			return element.index == input.index && element.semantic == input.semantic;
		} );
		if ( output == vertex.outputs.end() ) {
			return idStr( va( "the PS reads %s%u, which the VS does not write", input.semantic.c_str(), input.index ) );
		}
		if ( output->reg != input.reg || ( output->mask & input.mask ) != input.mask ) {
			return idStr( va( "%s%u is register %u mask 0x%x in the VS, %u mask 0x%x in the PS: declare the same struct", input.semantic.c_str(),
				input.index, output->reg, output->mask, input.reg, input.mask ) );
		}
	}
	return std::nullopt;
}

[[nodiscard]] std::expected<d3d12ShaderStage_t, idStr> StageOf( shaderStageType_t type, const char * name, d3d12HlslResult_t && built ) {
	if ( !built ) {
		return std::unexpected( built.error() );
	}
	d3d12ShaderStage_t stage;
	stage.type = type;
	stage.name = name;
	stage.info = built->info;
	stage.code = std::move( built->code );
	stage.reflection = std::move( built->reflection );
	return stage;
}

[[nodiscard]] size_t PrebuiltIndex( const renderProgStage_t & found, shaderStageType_t type ) {
	const size_t program = static_cast<size_t>( found.program - R_RenderProgs().data() );
	return ( program * 2 + ( found.skinned ? 1 : 0 ) ) * 2 + ( ( type == shaderStageType_t::VERTEX ) ? 0 : 1 );
}

}	// namespace

std::expected<d3d12ShaderStage_t, idStr> D3D12_BuildHlslStage( idD3D12HlslCompiler & compiler, shaderStageType_t type, const char * name,
	const d3d12HlslSource_t & source ) {
	return StageOf( type, name, compiler.Build( source, type ) );
}

// GL reflects the linked program: the masks of the PS, the inputs and the joints of the VS of the pair.
std::expected<d3d12Program_t, idStr> D3D12_LinkProgram( const d3d12ShaderStage_t & vertex, const d3d12ShaderStage_t * fragment ) {
	d3d12Program_t program;
	program.vertexCode = vertex.code;
	program.vertexInfo = vertex.info;
	if ( fragment != nullptr ) {
		if ( const std::optional<idStr> mismatch = SignatureMismatch( vertex.reflection, fragment->reflection ) ) {
			return std::unexpected( *mismatch );
		}
		program.fragmentCode = fragment->code;
		program.fragmentInfo = fragment->info;
		program.info.samplerMask = fragment->reflection.samplerMask;
		program.info.cubeMask = fragment->reflection.cubeMask;
	}
	program.info.attribMask = vertex.reflection.attribMask;
	program.info.usesJoints = vertex.reflection.usesJoints;
	program.journalName = vertex.name + "|" + ( ( fragment != nullptr ) ? fragment->name.c_str() : "-" );
	return program;
}

void idD3D12ShaderLibrary::Prebuild() {
	const std::span<const renderProgDef_t> table = R_RenderProgs();
	struct stage_t {
		size_t							index;
		size_t							source;
		shaderStageType_t				type;
	};
	std::vector<d3d12HlslSource_t> sources;
	std::vector<stage_t> wanted;
	for ( const renderProgDef_t & program : table ) {
		for ( const bool skinned : { false, true } ) {
			if ( skinned && program.skinned == renderProgBuiltin_t::NONE ) {
				continue;
			}
			// a program without a file is left to CreateStage, which warns
			std::optional<d3d12HlslSource_t> source = D3D12_FindHlslSource( R_RenderProgStageName( program, skinned ), compiler.Reader() );
			if ( !source ) {
				continue;
			}
			sources.push_back( std::move( *source ) );
			for ( const shaderStageType_t type : { shaderStageType_t::VERTEX, shaderStageType_t::FRAGMENT } ) {
				if ( type == shaderStageType_t::VERTEX || program.fragment ) {
					wanted.push_back( { PrebuiltIndex( { &program, skinned }, type ), sources.size() - 1, type } );
				}
			}
		}
	}
	std::vector<d3d12HlslRequest_t> requests;
	for ( const stage_t & stage : wanted ) {
		requests.push_back( { &sources[stage.source], stage.type } );
	}
	std::vector<d3d12HlslResult_t> built = compiler.BuildAll( requests );
	prebuilt.clear();
	prebuilt.resize( table.size() * 4 );
	for ( size_t i = 0; i < wanted.size(); i++ ) {
		prebuilt[wanted[i].index] = std::move( built[i] );
	}
}

// The program table decides which stages exist; a stage it lists is compiled from its file in base/shaders.
shaderHandle_t idD3D12ShaderLibrary::CreateStage( shaderStageType_t type, const char * name, shaderStageInfo_t & info ) {
	info.numParms = 0;
	const std::optional<renderProgStage_t> found = R_FindRenderProg( name );
	if ( !found || ( type == shaderStageType_t::FRAGMENT && !found->program->fragment ) ) {
		d3d12ShaderStage_t missing;
		missing.type = type;
		missing.name = name;
		missing.missing = true;
		return stages.Add( std::move( missing ) );
	}
	auto stage = [&]() -> std::expected<d3d12ShaderStage_t, idStr> {
		if ( const size_t index = PrebuiltIndex( *found, type ); index < prebuilt.size() && prebuilt[index] ) {
			d3d12HlslResult_t built = std::move( *prebuilt[index] );
			prebuilt[index].reset();
			return StageOf( type, name, std::move( built ) );
		}
		const std::optional<d3d12HlslSource_t> source = D3D12_FindHlslSource( name, compiler.Reader() );
		if ( !source ) {
			return std::unexpected( idStr( va( "%s has no file in base/shaders", name ) ) );
		}
		return D3D12_BuildHlslStage( compiler, type, name, *source );
	}();
	if ( !stage ) {
		common->Warning( "d3d12: %s", stage.error().c_str() );
		return shaderHandle_t::NONE;
	}
	info = stage->info;
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
	auto program = D3D12_LinkProgram( *vertexStage, fragmentStage );
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
	prebuilt.clear();
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

// Every stage of every shaders/*.hlsl, its SKINNED variant included, through the cache like a map load,
// so a later load finds what this compiled.
int idD3D12ShaderLibrary::CompileAll( bool verbose ) {
	idStrList failures;
	std::vector<d3d12HlslEntry_t> entries;
	idFileList * files = fileSystem->ListFiles( "shaders", ".hlsl", true );
	for ( int i = 0; i < files->GetNumFiles(); i++ ) {
		const idStr file = files->GetFile( i );
		const std::optional<d3d12HlslSource_t> plain = D3D12_FindHlslSource( file, compiler.Reader() );
		if ( !plain ) {
			continue;
		}
		auto fileEntries = D3D12_HlslEntries( compiler.Library(), *plain, compiler.Reader() );
		if ( !fileEntries ) {
			common->Printf( "d3d12_compileAll: %s\n", fileEntries.error().c_str() );
			failures.Append( file );
			continue;
		}
		std::ranges::move( *fileEntries, std::back_inserter( entries ) );
	}
	fileSystem->FreeFileList( files );
	std::vector<d3d12HlslRequest_t> requests;
	for ( const d3d12HlslEntry_t & entry : entries ) {
		requests.push_back( { &entry.source, entry.stage } );
	}
	std::vector<d3d12HlslResult_t> built = compiler.BuildAll( requests );
	for ( size_t i = 0; i < entries.size(); i++ ) {
		const char * name = entries[i].source.name.c_str();
		const char * suffix = ( entries[i].stage == shaderStageType_t::VERTEX ) ? "VSMain" : "PSMain";
		if ( !built[i] ) {
			common->Printf( "d3d12_compileAll: %s\n", built[i].error().c_str() );
			failures.Append( va( "%s %s", name, suffix ) );
		} else if ( verbose ) {
			common->Printf( "d3d12_compileAll: %s %s: %d parms, attribs 0x%04x, samplers 0x%04x\n", name, suffix,
				built[i]->info.numParms, built[i]->reflection.attribMask, built[i]->reflection.samplerMask );
		}
	}
	const int numStages = static_cast<int>( entries.size() );
	idStr list;
	for ( int i = 0; i < failures.Num(); i++ ) {
		list += ( i == 0 ) ? ": " : ", ";
		list += failures[i];
	}
	common->Printf( "d3d12_compileAll: %d stages, %d errors%s\n", numStages, failures.Num(), list.c_str() );
	return failures.Num();
}
