// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

/*
	The render programs in HLSL 2021 (base/shaders, research/hlsl/design.md), compiled by DXC at run time:

	  "interaction.vfp"          → shaders/interaction.hlsl, VSMain as vs_6_6, PSMain as ps_6_6
	  "interaction_skinned.vfp"  → shaders/interaction_skinned.hlsl, or shaders/interaction.hlsl with SKINNED=1

	A stage's parms are the members of its cbuffer, b0 for the VS and b1 for the PS: float4 each, named
	like renderParm_t ("rpMVPmatrixX", "rpUser0"), packed in their order. b2 is the joint block.
	DXIL is kept in fs_savepath/shaders/cache, keyed by the preprocessed text, the profile and DXC's version.

	BuildAll, a batch: preprocessing, the cache and the reflection run on the calling thread, the DXC runs
	the cache lacks run at once on worker threads, one run per distinct text and profile.
*/

#include "d3d12_Dxc.h"
#include "d3d12_ShaderCompiler.h"

// A stage's HLSL: the file, its text and the defines of the variant.
struct d3d12HlslSource_t {
	idStr							name;		// "interaction_skinned": messages and cache files
	idStr							path;		// "shaders/interaction.hlsl"
	idStr							text;
	idStrList						defines;	// "SKINNED=1"
};

// The source of a stage name the shared code uses ("gui.vfp", "heatHaze.vfp", "shadow.vp"); nullopt: no HLSL file.
[[nodiscard]] std::optional<d3d12HlslSource_t>	D3D12_FindHlslSource( const char * stageName, const d3d12ShaderReader_t & reader );
// One file by its path under base/, without defines; nullopt when it is missing.
[[nodiscard]] std::optional<d3d12HlslSource_t>	D3D12_LoadHlslFile( const char * path, const char * name, const d3d12ShaderReader_t & reader );

// The stages of a shaders/*.hlsl file: VSMain and PSMain where the preprocessed text (no comments) defines them, for
// the plain file and, when SKINNED=1 changes that text, for the "<name>_skinned" variant.
struct d3d12HlslEntry_t {
	d3d12HlslSource_t				source;
	shaderStageType_t				stage;
};
[[nodiscard]] std::expected<std::vector<d3d12HlslEntry_t>, idStr>	D3D12_HlslEntries( const idD3D12Library & library,
																		const d3d12HlslSource_t & plain, const d3d12ShaderReader_t & reader );

// RENDER_PARMS: the stage's cbuffer holds renderParm_t float4s (a render program); ANY: the backend's own passes.
enum class hlslParms_t { RENDER_PARMS, ANY };

// fileSystem: a loose file under base/ or one of the resource files.
[[nodiscard]] d3d12ShaderReader_t				D3D12_GameDataReader();

// A compiled stage: the signed DXIL, its parms and what its reflection says.
struct d3d12HlslStage_t {
	d3d12Bytecode_t					code;
	shaderStageInfo_t				info = {};
	d3d12StageReflection_t			reflection;
};

struct d3d12HlslRequest_t {
	const d3d12HlslSource_t *		source;
	shaderStageType_t				stage;
	hlslParms_t						parms = hlslParms_t::RENDER_PARMS;
};
using d3d12HlslResult_t = std::expected<d3d12HlslStage_t, idStr>;

class idD3D12HlslCompiler {
public:
	static constexpr int			VERSION = 1;	// in the cache key: bump it with every change of the arguments

	// threads: the most DXC runs at once, 0 = one per hardware thread.
									idD3D12HlslCompiler( const idD3D12Library & dlls, d3d12ShaderReader_t read, bool useCache, int threads = 0 ) :
										library( dlls ), reader( std::move( read ) ), cache( useCache, "shaders/cache", "dxil" ), maxThreads( threads ) {}

	// VSMain or PSMain through the cache. Errors are DXC's, or "name: what" for a cbuffer or binding the backend cannot fill.
	[[nodiscard]] d3d12HlslResult_t	Build( const d3d12HlslSource_t & source, shaderStageType_t stage,
										hlslParms_t parms = hlslParms_t::RENDER_PARMS );
	// Build of every request, the results in their order.
	[[nodiscard]] std::vector<d3d12HlslResult_t>	BuildAll( std::span<const d3d12HlslRequest_t> requests );
	[[nodiscard]] const d3d12ShaderReader_t &	Reader() const { return reader; }
	[[nodiscard]] const idD3D12Library &	Library() const { return library; }
	[[nodiscard]] int				Compiled() const { return compiled; }	// DXC runs
	[[nodiscard]] int				CacheHits() const { return hits; }
	[[nodiscard]] double			Milliseconds() const { return static_cast<double>( microseconds ) / 1000.0; }	// in Build and BuildAll

private:
	[[nodiscard]] uint64			Hash( std::string_view preprocessed, shaderStageType_t stage ) const;

	const idD3D12Library &			library;
	d3d12ShaderReader_t				reader;
	idD3D12BlobCache				cache;
	int								maxThreads;
	int								compiled = 0;
	int								hits = 0;
	uint64							microseconds = 0;
};
