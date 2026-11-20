// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

#include "d3d12_Device.h"

using d3d12Bytecode_t = std::vector<byte>;

// One row of a stage's input or output signature.
struct d3d12SignatureElement_t {
	idStr							semantic;			// upper case: "TEXCOORD"
	uint32							index = 0;
	uint32							reg = 0;
	uint8							mask = 0;			// the components the row declares
	D3D_NAME						systemValue = D3D_NAME_UNDEFINED;
};

// What GL's program queries report, read from one stage's DXBC or DXIL.
struct d3d12StageReflection_t {
	uint16							samplerMask = 0;	// PS: the bound textures tN
	uint16							cubeMask = 0;		// PS: those of them that are TextureCube
	uint16							attribMask = 0;		// VS: PC_ATTRIB_INDEX_* of the input signature rows
	bool							usesJoints = false;	// VS: the joint block b2 is bound
	std::vector<d3d12SignatureElement_t>	inputs;		// PS: what it reads from the VS
	std::vector<d3d12SignatureElement_t>	outputs;	// VS: what it writes for the PS
};

// The masks, the attributes, the joints and the signatures of a compiled stage.
[[nodiscard]] d3d12StageReflection_t	D3D12_ReflectStage( ID3D12ShaderReflection & reflection, shaderStageType_t stage );

// fs_savepath/<directory>/<name>.<vs|ps>.<hash>.<extension> through fileSystem; the hash covers the
// text, so a stale file is never read. Disabled, it touches no file (the tests have no fileSystem).
class idD3D12BlobCache {
public:
									idD3D12BlobCache( bool useFiles, const char * folder, const char * fileExtension ) :
										enabled( useFiles ), directory( folder ), extension( fileExtension ) {}
	[[nodiscard]] std::optional<d3d12Bytecode_t>	Find( const char * name, shaderStageType_t stage, uint64 hash ) const;
	void							Store( const char * name, shaderStageType_t stage, uint64 hash, std::span<const byte> code ) const;

private:
	[[nodiscard]] idStr				Path( const char * name, shaderStageType_t stage, uint64 hash ) const;

	bool							enabled;
	const char *					directory;
	const char *					extension;
};

// FXC (d3dcompiler_47.dll from System32): vs_5_1 / ps_5_1 with the entry points vsMain and psMain;
// every compile goes through the cache. r_d3d12DumpHLSL 1 writes each text to renderprogs/hlsl/.
class idD3D12ShaderCompiler {
public:
	// Every config and debug level: debug flags change pixels, and a GBV run must give the same frames.
	static constexpr UINT			FLAGS = D3DCOMPILE_OPTIMIZATION_LEVEL3;

									idD3D12ShaderCompiler( const idD3D12Library & dlls, bool useCache ) : library( dlls ), cache( useCache, "renderprogs/dxbc", "dxbc" ) {}
	// name is for the messages and the cache file. Warnings go to the developer console, errors into the result.
	[[nodiscard]] std::expected<d3d12Bytecode_t, idStr>	Compile( std::string_view hlsl, shaderStageType_t stage, const char * name );
	[[nodiscard]] d3d12StageReflection_t	Reflect( std::span<const byte> code, shaderStageType_t stage ) const;
	[[nodiscard]] int				Compiled() const { return compiled; }	// FXC runs
	[[nodiscard]] int				CacheHits() const { return hits; }

private:
	// MD5 folded to 64 bits of: the text, profile, entry, FLAGS, compilerVersion, idD3D12HlslEmitter::VERSION.
	[[nodiscard]] uint64			Hash( std::string_view hlsl, shaderStageType_t stage ) const;

	const idD3D12Library &			library;
	idD3D12BlobCache				cache;
	int								compiled = 0;
	int								hits = 0;
};
