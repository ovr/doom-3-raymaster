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

#include "d3d12_Device.h"

using d3d12Bytecode_t = std::vector<byte>;

// What GL's program queries report, read from one stage's DXBC.
struct d3d12StageReflection_t {
	uint16							samplerMask = 0;	// PS: the bound textures tN
	uint16							cubeMask = 0;		// PS: those of them that are TextureCube
	uint16							attribMask = 0;		// VS: PC_ATTRIB_INDEX_* of the input signature rows
	bool							usesJoints = false;	// VS: matrices_ubo is bound
};

// fs_savepath/renderprogs/dxbc/<name>.<vs|ps>.<hash>.dxbc through fileSystem; the hash covers the
// text, so a stale file is never read. Disabled, it touches no file (the tests have no fileSystem).
class idD3D12BlobCache {
public:
	explicit						idD3D12BlobCache( bool useFiles ) : enabled( useFiles ) {}
	[[nodiscard]] std::optional<d3d12Bytecode_t>	Find( const char * name, shaderStageType_t stage, uint64 hash ) const;
	void							Store( const char * name, shaderStageType_t stage, uint64 hash, std::span<const byte> code ) const;

private:
	bool							enabled;
};

// FXC (d3dcompiler_47.dll from System32): vs_5_1 / ps_5_1 with the entry points vsMain and psMain;
// every compile goes through the cache. r_d3d12DumpHLSL 1 writes each text to renderprogs/hlsl/.
class idD3D12ShaderCompiler {
public:
	// Every config and debug level: debug flags change pixels, and a GBV run must give the same frames.
	static constexpr UINT			FLAGS = D3DCOMPILE_OPTIMIZATION_LEVEL3;

									idD3D12ShaderCompiler( const idD3D12Library & dlls, bool useCache ) : library( dlls ), cache( useCache ) {}
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
