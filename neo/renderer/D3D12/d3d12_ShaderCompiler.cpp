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

#pragma hdrstop
#include "../../idlib/precompiled.h"

#include "../tr_local.h"
#include "d3d12_ShaderCompiler.h"

namespace {

idStr BlobText( ID3DBlob * blob ) {
	if ( blob == nullptr || blob->GetBufferSize() == 0 ) {
		return idStr();
	}
	idStr text;
	text.Append( static_cast<const char *>( blob->GetBufferPointer() ), static_cast<int>( blob->GetBufferSize() ) );
	text.StripTrailingWhitespace();
	return text;
}

}	// namespace

std::expected<d3d12Bytecode_t, idStr> idD3D12ShaderCompiler::Compile( std::string_view hlsl, shaderStageType_t stage, const char * name ) {
	const bool vertex = ( stage == shaderStageType_t::VERTEX );
	idComPtr<ID3DBlob> code;
	idComPtr<ID3DBlob> messages;
	const HRESULT hr = library.compile( hlsl.data(), hlsl.size(), name, nullptr, nullptr, vertex ? "vsMain" : "psMain",
		vertex ? "vs_5_1" : "ps_5_1", FLAGS, 0, &code, &messages );
	const idStr text = BlobText( messages.Get() );
	if ( FAILED( hr ) || !code ) {
		return std::unexpected( idStr( va( "%s: %s", name, text.Length() > 0 ? text.c_str() : D3D12_ErrorText( hr ).c_str() ) ) );
	}
	if ( text.Length() > 0 ) {
		common->DPrintf( "d3d12: %s: %s\n", name, text.c_str() );
	}
	compiled++;
	const byte * bytes = static_cast<const byte *>( code->GetBufferPointer() );
	return d3d12Bytecode_t( bytes, bytes + code->GetBufferSize() );
}
