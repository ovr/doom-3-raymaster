// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

/*
	The only include of the Direct3D 12, DXGI and DXC headers. Every file of renderer/D3D12/
	includes precompiled.h first and this header after it. No file outside this folder (and
	neo/tests/d3d12_*) names these APIs: tools/check-render-api-leaks.ps1 fails when one does.
	d3d12.h is the Agility SDK's (external/d3d12-x64, tools/d3d12/install-d3d12-sdk.ps1).

	The DLLs are never linked: idD3D12Library loads them at Init. Windows' own (d3d12, dxgi) come
	from System32 only, the ones that ship with the exe (dxcompiler and dxil, the Agility runtime in
	D3D12\) from its folder only.
*/

#include <d3d12.h>
#include <dxgi1_6.h>
#include <dxgidebug.h>
#include <d3d12shader.h>
#include <dxcapi.h>
#include <wrl/client.h>

#include <array>
#include <bit>
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
enum class d3d12RootSlot_t : UINT { VERTEX_PARMS, FRAGMENT_PARMS, JOINTS, DRAW_RESOURCES, TRACE_RESOURCES, COUNT };
[[nodiscard]] constexpr UINT D3D12_Root( d3d12RootSlot_t slot ) { return static_cast<UINT>( slot ); }

// The compute root signature of the backend's own compute passes: root CBV b0, then root constants b1.
enum class d3d12ComputeSlot_t : UINT { PARMS, CONSTANTS, COUNT };
[[nodiscard]] constexpr UINT D3D12_ComputeRoot( d3d12ComputeSlot_t slot ) { return static_cast<UINT>( slot ); }
struct d3d12ComputeConstants_t {
	static constexpr UINT			COUNT = 8;		// 32-bit values at b1: what changes between dispatches of one pass
};

// colorFlags of b3 (include/colorspace.hlsli): which slots hold a float copy of the HDR scene, the HDR phase
// (R13), and whether the draw's output is radiance that the blend adds, not a factor it multiplies by.
namespace d3d12ColorFlags {
constexpr uint32					FLOAT_SLOTS = 0xFF;
constexpr uint32					HDR_PHASE = 1u << 8;
constexpr uint32					RADIANCE = 1u << 9;
constexpr uint32					GAMMA_PHASE = 1u << 10;
}

// The root constants b3 of base/shaders/include/bindless.hlsli, in its member order: the heap index of each
// texture slot's 2D view, then of its cube view, then of its sampler; then colorFlags and preExposure. A slot
// the program does not read stays 0.
struct d3d12DrawResources_t {
	static constexpr UINT			REGISTER = 3;
	static constexpr uint32			CUBES = MAX_TEXTURE_SLOTS;			// textureCube0
	static constexpr uint32			SAMPLERS = 2 * MAX_TEXTURE_SLOTS;	// sampler0
	static constexpr uint32			SLOTS = 3 * MAX_TEXTURE_SLOTS;		// the views and samplers of the slots
	static constexpr uint32			FLAGS = SLOTS;						// colorFlags
	static constexpr uint32			PRE_EXPOSURE = SLOTS + 1;			// float
	static constexpr uint32			COUNT = SLOTS + 2;
	std::array<uint32, COUNT>		indices = {};
	constexpr bool					operator==( const d3d12DrawResources_t & ) const = default;
	void							SetTexture( int slot, bool cube, uint32 view ) { indices[( cube ? CUBES : 0 ) + slot] = view; }
	void							SetSampler( int slot, uint32 sampler ) { indices[SAMPLERS + slot] = sampler; }
	void							SetColor( uint32 flags, float preExposure ) {
		indices[FLAGS] = flags;
		indices[PRE_EXPOSURE] = std::bit_cast<uint32>( preExposure );
	}
};

// The root constants b4 of base/shaders/include/rtDraw.hlsli, in its member order, for the pixel stage of a render
// program that traces: the heap indices of what a ray of the GPU scene reads (R15), of the blue noise and of the
// sampler of the perforated casters, then flags. All zero while nothing may be traced.
struct d3d12TraceResources_t {
	static constexpr UINT			REGISTER = 4;
	static constexpr uint32			TLAS = 0;
	static constexpr uint32			RECORDS = 1;
	static constexpr uint32			INSTANCES = 2;
	static constexpr uint32			VERTICES = 3;		// the static vertex buffer
	static constexpr uint32			INDEXES = 4;		// the static index buffer
	static constexpr uint32			SKINNED = 5;		// the frame's skinned vertices
	static constexpr uint32			NOISE = 6;			// renderer/BlueNoise.h as a texture
	static constexpr uint32			SAMPLER = 7;		// linear, repeating
	static constexpr uint32			FLAGS = 8;
	static constexpr uint32			COUNT = 9;
	static constexpr uint32			VALID = 1u << 0;	// of FLAGS: the indices name what the last applied update built
	std::array<uint32, COUNT>		indices = {};
	constexpr bool					operator==( const d3d12TraceResources_t & ) const = default;
	[[nodiscard]] constexpr bool	Valid() const { return ( indices[FLAGS] & VALID ) != 0; }
};

// "0x887A0005 DXGI_ERROR_DEVICE_REMOVED", or the bare value for a code without a name here.
[[nodiscard]] idStr D3D12_ErrorText( HRESULT hr );

class idD3D12Submitter;
struct d3d12Features_t;
