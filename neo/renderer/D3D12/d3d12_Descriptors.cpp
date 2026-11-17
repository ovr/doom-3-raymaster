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

#include <algorithm>
#include <functional>

#include "../tr_local.h"
#include "d3d12_Descriptors.h"
#include "d3d12_StateMap.h"

namespace {

idComPtr<ID3D12DescriptorHeap> CreateHeap( const idD3D12Device & device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32 count,
	D3D12_DESCRIPTOR_HEAP_FLAGS flags, const wchar_t * name ) {
	D3D12_DESCRIPTOR_HEAP_DESC desc = {};
	desc.Type = type;
	desc.NumDescriptors = count;
	desc.Flags = flags;
	idComPtr<ID3D12DescriptorHeap> heap;
	if ( device.Check( device.Get().CreateDescriptorHeap( &desc, IID_PPV_ARGS( &heap ) ), "CreateDescriptorHeap" ) ) {
		heap->SetName( name );
	}
	return heap;
}

const wchar_t * PoolName( D3D12_DESCRIPTOR_HEAP_TYPE type ) {
	switch ( type ) {
		case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:	return L"doom3 RTV pool";
		case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:	return L"doom3 DSV pool";
		default:								return L"doom3 SRV pool";
	}
}

}	// namespace

idD3D12DescriptorPool::idD3D12DescriptorPool( const idD3D12Device & owner, D3D12_DESCRIPTOR_HEAP_TYPE heapType ) :
	device( owner ),
	type( heapType ),
	increment( owner.Get().GetDescriptorHandleIncrementSize( heapType ) ) {
}

d3d12Descriptor_t idD3D12DescriptorPool::Allocate() {
	if ( !freeSlots.empty() ) {
		std::pop_heap( freeSlots.begin(), freeSlots.end(), std::greater<>() );
		const uint32 slot = freeSlots.back();
		freeSlots.pop_back();
		return { slot };
	}
	if ( next == heaps.size() * HEAP_SIZE ) {
		heaps.push_back( CreateHeap( device, type, HEAP_SIZE, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, PoolName( type ) ) );
		starts.push_back( heaps.back() ? heaps.back()->GetCPUDescriptorHandleForHeapStart() : D3D12_CPU_DESCRIPTOR_HANDLE{} );
	}
	return { next++ };
}

void idD3D12DescriptorPool::Free( d3d12Descriptor_t slot ) {
	assert( slot.IsValid() && slot.index < next );
	freeSlots.push_back( slot.index );
	std::push_heap( freeSlots.begin(), freeSlots.end(), std::greater<>() );
}

D3D12_CPU_DESCRIPTOR_HANDLE idD3D12DescriptorPool::Cpu( d3d12Descriptor_t slot ) const {
	assert( slot.IsValid() && slot.index < next );
	D3D12_CPU_DESCRIPTOR_HANDLE handle = starts[slot.index / HEAP_SIZE];
	handle.ptr += static_cast<SIZE_T>( slot.index % HEAP_SIZE ) * increment;
	return handle;
}

idD3D12DescriptorRing::idD3D12DescriptorRing( const idD3D12Device & owner ) :
	device( owner ),
	heap( CreateHeap( owner, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SIZE, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, L"doom3 shader-visible SRV ring" ) ),
	increment( owner.Get().GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ) ),
	ring( SIZE ) {
	if ( heap ) {
		cpuStart = heap->GetCPUDescriptorHandleForHeapStart();
		gpuStart = heap->GetGPUDescriptorHandleForHeapStart();
	}
}

std::optional<D3D12_GPU_DESCRIPTOR_HANDLE> idD3D12DescriptorRing::TryCopyTable( std::span<const D3D12_CPU_DESCRIPTOR_HANDLE, MAX_TEXTURE_SLOTS> sources ) {
	const std::optional<uint64> slot = ring.TryAllocate( MAX_TEXTURE_SLOTS, 1 );
	if ( !slot || !heap ) {
		return std::nullopt;
	}
	D3D12_CPU_DESCRIPTOR_HANDLE destination = cpuStart;
	destination.ptr += static_cast<SIZE_T>( *slot ) * increment;
	constexpr UINT tableSize = MAX_TEXTURE_SLOTS;
	std::array<UINT, MAX_TEXTURE_SLOTS> sourceSizes;
	sourceSizes.fill( 1 );
	device.Get().CopyDescriptors( 1, &destination, &tableSize, tableSize, sources.data(), sourceSizes.data(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV );
	D3D12_GPU_DESCRIPTOR_HANDLE table = gpuStart;
	table.ptr += *slot * increment;
	return table;
}

size_t d3d12SamplerTableHash_t::operator()( const d3d12SamplerTableKey_t & key ) const {
	uint64 hash = 0xCBF29CE484222325ull;
	for ( const uint64 sampler : key.samplers ) {
		hash = ( hash ^ sampler ) * 0x100000001B3ull;
		hash ^= hash >> 29;
	}
	return static_cast<size_t>( hash );
}

idD3D12SamplerHeap::idD3D12SamplerHeap( const idD3D12Device & owner ) :
	device( owner ),
	heap( CreateHeap( owner, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, TABLES * MAX_TEXTURE_SLOTS, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
		L"doom3 shader-visible sampler tables" ) ),
	increment( owner.Get().GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ) ) {
	if ( heap ) {
		cpuStart = heap->GetCPUDescriptorHandleForHeapStart();
		gpuStart = heap->GetGPUDescriptorHandleForHeapStart();
	}
	lookup.reserve( TABLES );
}

std::optional<D3D12_GPU_DESCRIPTOR_HANDLE> idD3D12SamplerHeap::TryTable( const d3d12SamplerTableKey_t & key, uint64 useFence, uint64 waitedFence ) {
	if ( !heap ) {
		return std::nullopt;
	}
	uint32 index = TABLES;
	if ( const auto found = lookup.find( key ); found != lookup.end() ) {
		index = found->second;
	} else {
		if ( used < TABLES ) {
			index = used++;
		} else {
			uint64 oldest = UINT64_MAX;
			for ( uint32 i = 0; i < TABLES; i++ ) {
				if ( tables[i].lastUse <= waitedFence && tables[i].lastUse < oldest ) {
					index = i;
					oldest = tables[i].lastUse;
				}
			}
			if ( index == TABLES ) {
				return std::nullopt;
			}
			lookup.erase( tables[index].key );
		}
		Build( index, key );
		lookup.emplace( key, index );
	}
	tables[index].lastUse = Max( tables[index].lastUse, useFence );
	D3D12_GPU_DESCRIPTOR_HANDLE table = gpuStart;
	table.ptr += static_cast<uint64>( index ) * MAX_TEXTURE_SLOTS * increment;
	return table;
}

uint64 idD3D12SamplerHeap::OldestUse() const {
	uint64 oldest = 0;
	for ( uint32 i = 0; i < used; i++ ) {
		oldest = ( i == 0 ) ? tables[i].lastUse : Min( oldest, tables[i].lastUse );
	}
	return oldest;
}

void idD3D12SamplerHeap::Build( uint32 index, const d3d12SamplerTableKey_t & key ) {
	tables[index].key = key;
	tables[index].lastUse = 0;
	D3D12_CPU_DESCRIPTOR_HANDLE slot = cpuStart;
	slot.ptr += static_cast<SIZE_T>( index ) * MAX_TEXTURE_SLOTS * increment;
	for ( const uint64 sampler : key.samplers ) {
		const D3D12_SAMPLER_DESC desc = D3D12_SamplerFor( sampler, device.Features().anisoPointMip );
		device.Get().CreateSampler( &desc, slot );
		slot.ptr += increment;
	}
	built++;
}
