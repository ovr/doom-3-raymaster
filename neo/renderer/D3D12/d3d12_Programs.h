// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

#include "d3d12_HandleTable.h"
#include "d3d12_HlslCompiler.h"

struct d3d12ShaderStage_t {
	shaderStageType_t				type = shaderStageType_t::VERTEX;
	idStr							name;				// as shared code names it: "gui.vfp"
	bool							missing = false;	// the game data lacks it: CreateProgram raises GL's link Error
	shaderStageInfo_t				info = {};
	d3d12Bytecode_t					code;				// the DXIL of base/shaders
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
	bool							colorConstants = false;	// a stage reads colorFlags or preExposure of b3
	uint16							dense = 0;		// pipelineKey_t::program
	idStr							journalName;	// "<vs>|<ps>": stable across runs, unlike handles
	std::vector<d3d12Pipeline_t>	pipelines;		// 1-20 keys: a linear search beats a hash
};

// The device-free halves of CreateStage and CreateProgram, which the tests run on the repo's sources.
// A stage of base/shaders; errors are DXC's or "name: what".
[[nodiscard]] std::expected<d3d12ShaderStage_t, idStr>	D3D12_BuildHlslStage( idD3D12HlslCompiler & compiler, shaderStageType_t type, const char * name,
															const d3d12HlslSource_t & source );
// fragment nullptr: a program without a PS. The PS must read what the VS writes at the same registers.
[[nodiscard]] std::expected<d3d12Program_t, idStr>		D3D12_LinkProgram( const d3d12ShaderStage_t & vertex, const d3d12ShaderStage_t * fragment );

// shaderHandle_t → d3d12ShaderStage_t, programHandle_t → d3d12Program_t.
class idD3D12ShaderLibrary {
public:
									idD3D12ShaderLibrary( idD3D12HlslCompiler & dxc, idD3D12Graveyard & graves ) : compiler( dxc ), graveyard( graves ) {}
	// Every stage of the program table in one BuildAll, kept until CreateStage asks for it. The first frame drops
	// what Init did not ask for: a stage made later is built again, so an edited file counts (its DXIL is cached).
	void							Prebuild();
	void							DropPrebuilt();
	// A stage the game data lacks gets a missing record, as GL does; one without a base/shaders file or with
	// a compile error returns NONE.
	[[nodiscard]] shaderHandle_t	CreateStage( shaderStageType_t stage, const char * name, shaderStageInfo_t & info );
	// A missing stage raises idLib::Error with GL's link text; a vertex stage NONE gives NONE.
	[[nodiscard]] programHandle_t	CreateProgram( shaderHandle_t vertex, shaderHandle_t fragment, const char * name, programInfo_t & info );
	// A linked program under a new handle and dense index (CreateProgram, the tests); NONE on a full table.
	[[nodiscard]] programHandle_t	AddProgram( d3d12Program_t && program );
	void							DestroyStage( shaderHandle_t stage );
	void							DestroyProgram( programHandle_t program );	// buries its PSOs
	[[nodiscard]] d3d12Program_t &	Program( programHandle_t program ) { return programs.Get( program ); }
	[[nodiscard]] d3d12Program_t *	FindProgram( programHandle_t program ) { return programs.Find( program ); }
	// d3d12_compileAll: every stage of shaders/*.hlsl, the errors printed; returns their number.
	int								CompileAll( bool verbose );
	void							DestroyAll();
	[[nodiscard]] int				NumStages() const { return stages.Num(); }
	[[nodiscard]] int				NumPrograms() const { return programs.Num(); }

private:
	[[nodiscard]] uint16			NewDense();
	void							FreeDense( uint16 dense );

	idD3D12HlslCompiler &			compiler;
	idD3D12Graveyard &				graveyard;
	idD3D12HandleTable<d3d12ShaderStage_t, shaderHandle_t>	stages;
	idD3D12HandleTable<d3d12Program_t, programHandle_t>		programs;
	std::vector<uint16>				freeDense;		// a min-heap of the dense indexes below nextDense
	uint16							nextDense = 0;
	std::vector<std::optional<d3d12HlslResult_t>>	prebuilt;	// by PrebuiltIndex, taken by CreateStage
};
