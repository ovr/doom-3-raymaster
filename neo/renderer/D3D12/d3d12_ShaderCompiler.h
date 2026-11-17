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

// FXC (d3dcompiler_47.dll from System32): vs_5_1 / ps_5_1 with the entry points vsMain and psMain.
class idD3D12ShaderCompiler {
public:
	// Every config and debug level: debug flags change pixels, and a GBV run must give the same frames.
	static constexpr UINT			FLAGS = D3DCOMPILE_OPTIMIZATION_LEVEL3;

	explicit						idD3D12ShaderCompiler( const idD3D12Library & dlls ) : library( dlls ) {}
	// name is for the messages only. Warnings go to the developer console, errors into the result.
	[[nodiscard]] std::expected<d3d12Bytecode_t, idStr>	Compile( std::string_view hlsl, shaderStageType_t stage, const char * name );
	[[nodiscard]] int				Compiled() const { return compiled; }

private:
	const idD3D12Library &			library;
	int								compiled = 0;
};
