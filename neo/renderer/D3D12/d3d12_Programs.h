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

#include "d3d12_HandleTable.h"
#include "d3d12_HlslEmitter.h"
#include "d3d12_ShaderCompiler.h"

struct d3d12ShaderStage_t {
	shaderStageType_t				type = shaderStageType_t::VERTEX;
	idStr							name;				// as shared code names it: "gui.vfp"
	bool							missing = false;	// the game data lacks it: CreateProgram raises GL's link Error
	d3d12EmittedStage_t				emitted;
	d3d12Bytecode_t					code;				// compiled with its own varyings
	d3d12StageReflection_t			reflection;
};

struct d3d12Pipeline_t {
	pipelineKey_t					key = {};
	idComPtr<ID3D12PipelineState>	pso;
};

// What a programHandle_t names; its PSOs die with it.
struct d3d12Program_t {
	d3d12Bytecode_t					vertexCode;		// own copies: KillAllShaders destroys the stages before the programs
	d3d12Bytecode_t					fragmentCode;	// empty for shadow.vp and shadow_skinned.vp: a PSO without a PS
	shaderStageInfo_t				vertexInfo = {};
	shaderStageInfo_t				fragmentInfo = {};
	programInfo_t					info = {};
	uint16							dense = 0;		// pipelineKey_t::program
	idStr							journalName;	// "<vs>|<ps>": stable across runs, unlike handles
	std::vector<d3d12Pipeline_t>	pipelines;		// 1-20 keys: a linear search beats a hash
};

// The device-free halves of CreateStage and CreateProgram, which the tests run on the repo's sources.
// A stage emitted and compiled with its own varyings; errors are "name: what".
[[nodiscard]] std::expected<d3d12ShaderStage_t, idStr>	D3D12_BuildStage( idD3D12ShaderCompiler & compiler, shaderStageType_t type, const char * name,
															const renderProgSource_t & source );
// fragment nullptr: a program without a PS. The VS is compiled again when the PS reads other varyings.
[[nodiscard]] std::expected<d3d12Program_t, idStr>		D3D12_LinkProgram( idD3D12ShaderCompiler & compiler, const d3d12ShaderStage_t & vertex,
															const d3d12ShaderStage_t * fragment );

// shaderHandle_t → d3d12ShaderStage_t, programHandle_t → d3d12Program_t.
class idD3D12ShaderLibrary {
public:
									idD3D12ShaderLibrary( idD3D12ShaderCompiler & fxc, idD3D12Graveyard & graves ) : compiler( fxc ), graveyard( graves ) {}
	// A stage the game data lacks gets a missing record, as GL does; an emit or compile error returns NONE.
	[[nodiscard]] shaderHandle_t	CreateStage( shaderStageType_t stage, const char * name, shaderStageInfo_t & info );
	// A missing stage raises idLib::Error with GL's link text; a vertex stage NONE gives NONE.
	[[nodiscard]] programHandle_t	CreateProgram( shaderHandle_t vertex, shaderHandle_t fragment, const char * name, programInfo_t & info );
	void							DestroyStage( shaderHandle_t stage );
	void							DestroyProgram( programHandle_t program );	// buries its PSOs
	[[nodiscard]] d3d12Program_t &	Program( programHandle_t program ) { return programs.Get( program ); }
	[[nodiscard]] d3d12Program_t *	FindProgram( programHandle_t program ) { return programs.Find( program ); }
	// d3d12_compileAll: every renderprogs/*.vertex|.pixel, the errors printed; returns their number.
	int								CompileAll( bool verbose );
	void							DestroyAll();
	[[nodiscard]] int				NumStages() const { return stages.Num(); }
	[[nodiscard]] int				NumPrograms() const { return programs.Num(); }

private:
	[[nodiscard]] uint16			NewDense();
	void							FreeDense( uint16 dense );

	idD3D12ShaderCompiler &			compiler;
	idD3D12Graveyard &				graveyard;
	idD3D12HandleTable<d3d12ShaderStage_t, shaderHandle_t>	stages;
	idD3D12HandleTable<d3d12Program_t, programHandle_t>		programs;
	std::vector<uint16>				freeDense;		// a min-heap of the dense indexes below nextDense
	uint16							nextDense = 0;
};
