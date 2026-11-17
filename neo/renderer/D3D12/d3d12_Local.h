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

/*
	The only include of the Direct3D 12, DXGI and D3DCompiler headers. Every file of renderer/D3D12/
	includes precompiled.h first and this header after it. No file outside this folder (and
	neo/tests/d3d12_*) names these APIs: tools/check-render-api-leaks.ps1 fails when one does.

	The DLLs are never linked: idD3D12Library loads them from System32 at Init, so an OpenGL run
	does not load them through this code.
*/

#include <d3d12.h>
#include <dxgi1_6.h>
#include <dxgidebug.h>
#include <d3dcompiler.h>
#include <d3d12shader.h>
#include <wrl/client.h>

#include <array>
#include <deque>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

template<typename T> using idComPtr = Microsoft::WRL::ComPtr<T>;

struct d3d12HandleCloser_t { void operator()( HANDLE h ) const { if ( h != nullptr ) { CloseHandle( h ); } } };
using idD3D12Event = std::unique_ptr<std::remove_pointer_t<HANDLE>, d3d12HandleCloser_t>;

struct d3d12ModuleFreer_t { void operator()( HMODULE m ) const { if ( m != nullptr ) { FreeLibrary( m ); } } };
using idD3D12Module = std::unique_ptr<std::remove_pointer_t<HMODULE>, d3d12ModuleFreer_t>;

// Heap.cpp memory must never reach free(): CPU blocks come from Mem_Alloc and go back through Mem_Free.
struct d3d12MemFree_t { void operator()( byte * p ) const { Mem_Free( p ); } };
using idD3D12CpuBlock = std::unique_ptr<byte[], d3d12MemFree_t>;

// The root parameter indices of the one root signature.
enum class d3d12RootSlot_t : UINT { VERTEX_PARMS, FRAGMENT_PARMS, JOINTS, TEXTURES, SAMPLERS, COUNT };
[[nodiscard]] constexpr UINT D3D12_Root( d3d12RootSlot_t slot ) { return static_cast<UINT>( slot ); }

// Until the backend is complete: warns once per name and counts the calls. Owner thread only.
void D3D12_Unimplemented( const char * what );
// One line with every name D3D12_Unimplemented has seen and its call count; nothing when there is none.
void D3D12_PrintUnimplemented();

// "0x887A0005 DXGI_ERROR_DEVICE_REMOVED", or the bare value for a code without a name here.
[[nodiscard]] idStr D3D12_ErrorText( HRESULT hr );

class idD3D12Submitter;
struct d3d12Features_t;
