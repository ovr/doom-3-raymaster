// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

/*
	The only include of the Direct3D 12, DXGI and DXC headers. Every file of renderer/D3D12/
	includes precompiled.h first and this header after it. No file outside this folder (and
	neo/tests/d3d12_*) names these APIs: tools/check-render-api-leaks.ps1 fails when one does.
	d3d12.h is the Agility SDK's (external/d3d12-x64, tools/d3d12/install-d3d12-sdk.ps1).

	The DLLs are never linked: idD3D12Library loads them at Init, so an OpenGL run does not load
	them through this code. Windows' own (d3d12, dxgi) come from System32 only, the ones that ship
	with the exe (dxcompiler and dxil, the Agility runtime in D3D12\) from its folder only.
*/

#include <d3d12.h>
#include <dxgi1_6.h>
#include <dxgidebug.h>
#include <d3d12shader.h>
#include <dxcapi.h>
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

// "0x887A0005 DXGI_ERROR_DEVICE_REMOVED", or the bare value for a code without a name here.
[[nodiscard]] idStr D3D12_ErrorText( HRESULT hr );

class idD3D12Submitter;
struct d3d12Features_t;
