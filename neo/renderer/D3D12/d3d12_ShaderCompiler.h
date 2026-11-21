// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

#include "d3d12_Local.h"

using d3d12Bytecode_t = std::vector<byte>;

// The entry point DXC compiles for the stage.
[[nodiscard]] constexpr const char * D3D12_StageEntry( shaderStageType_t stage ) {
	switch ( stage ) {
		case shaderStageType_t::VERTEX:		return "VSMain";
		case shaderStageType_t::FRAGMENT:	return "PSMain";
		default:							return "CSMain";
	}
}

// One row of a stage's input or output signature.
struct d3d12SignatureElement_t {
	idStr							semantic;			// upper case: "TEXCOORD"
	uint32							index = 0;
	uint32							reg = 0;
	uint8							mask = 0;			// the components the row declares
	D3D_NAME						systemValue = D3D_NAME_UNDEFINED;
};

// What GL's program queries report, read from one stage's DXIL.
struct d3d12StageReflection_t {
	uint16							samplerMask = 0;	// PS: the texture slots it reads (texture2D<n>, textureCube<n> of b3)
	uint16							cubeMask = 0;		// PS: those of them that are cubes
	uint16							samplerReads = 0;	// PS: the sampler<n> of b3 it reads
	uint16							attribMask = 0;		// VS: PC_ATTRIB_INDEX_* of the input signature rows
	bool							usesJoints = false;	// VS: the joint block b2 is bound
	std::array<uint32, 3>			threadGroup = {};	// CS: numthreads
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
