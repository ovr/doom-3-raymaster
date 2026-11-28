// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

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

idD3D12DescriptorPool::idD3D12DescriptorPool( const idD3D12Device & owner, D3D12_DESCRIPTOR_HEAP_TYPE heapType, uint32 shaderVisible ) :
	device( owner ),
	type( heapType ),
	increment( owner.Get().GetDescriptorHandleIncrementSize( heapType ) ),
	visibleSize( shaderVisible ) {
	assert( shaderVisible == 0 || heapType == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV );
	if ( shaderVisible > 0 ) {
		visible = CreateHeap( owner, heapType, shaderVisible, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, L"doom3 shader-visible SRV heap" );
		if ( visible ) {
			visibleStart = visible->GetCPUDescriptorHandleForHeapStart();
			visibleGpuStart = visible->GetGPUDescriptorHandleForHeapStart();
		}
	}
}

d3d12Descriptor_t idD3D12DescriptorPool::Allocate() {
	if ( !freeSlots.empty() ) {
		std::pop_heap( freeSlots.begin(), freeSlots.end(), std::greater<>() );
		const uint32 slot = freeSlots.back();
		freeSlots.pop_back();
		return { slot };
	}
	if ( visibleSize > 0 && next == visibleSize ) {
		common->FatalError( "d3d12: more than %u shader resource views live at once", visibleSize );
	}
	if ( next == heaps.size() * HEAP_SIZE ) {
		heaps.push_back( CreateHeap( device, type, HEAP_SIZE, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, PoolName( type ) ) );
		starts.push_back( heaps.back() ? heaps.back()->GetCPUDescriptorHandleForHeapStart() : D3D12_CPU_DESCRIPTOR_HANDLE{} );
	}
	return { next++ };
}

d3d12Descriptor_t idD3D12DescriptorPool::AllocateRange( uint32 count ) {
	assert( count > 0 );
	for ( auto it = freeRanges.begin(); it != freeRanges.end(); ++it ) {
		if ( it->second >= count ) {
			const uint32 first = it->first;
			it->first += count;
			it->second -= count;
			if ( it->second == 0 ) {
				freeRanges.erase( it );
			}
			return { first };
		}
	}
	if ( visibleSize > 0 && next + count > visibleSize ) {
		common->FatalError( "d3d12: more than %u shader resource views live at once", visibleSize );
	}
	const uint32 first = next;
	for ( uint32 i = 0; i < count; i++ ) {
		if ( next == heaps.size() * HEAP_SIZE ) {
			heaps.push_back( CreateHeap( device, type, HEAP_SIZE, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, PoolName( type ) ) );
			starts.push_back( heaps.back() ? heaps.back()->GetCPUDescriptorHandleForHeapStart() : D3D12_CPU_DESCRIPTOR_HANDLE{} );
		}
		next++;
	}
	return { first };
}

// Sorted, with neighbours merged: tables of every size come back as one range.
void idD3D12DescriptorPool::FreeRange( d3d12Descriptor_t first, uint32 count ) {
	assert( first.IsValid() && count > 0 && first.index + count <= next );
	auto it = std::ranges::lower_bound( freeRanges, first.index, {}, &std::pair<uint32, uint32>::first );
	assert( it == freeRanges.end() || first.index + count <= it->first );
	if ( it != freeRanges.end() && first.index + count == it->first ) {
		it->first = first.index;
		it->second += count;
	} else {
		it = freeRanges.insert( it, { first.index, count } );
	}
	if ( it != freeRanges.begin() ) {
		const auto before = std::prev( it );
		if ( before->first + before->second == it->first ) {
			before->second += it->second;
			freeRanges.erase( it );
		}
	}
}

void idD3D12DescriptorPool::Copy( d3d12Descriptor_t destination, d3d12Descriptor_t source ) {
	device.Get().CopyDescriptorsSimple( 1, Cpu( destination ), Cpu( source ), type );
	Mirror( destination );
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

void idD3D12DescriptorPool::CreateSrv( d3d12Descriptor_t slot, ID3D12Resource & resource, const D3D12_SHADER_RESOURCE_VIEW_DESC & desc ) {
	device.Get().CreateShaderResourceView( &resource, &desc, Cpu( slot ) );
	Mirror( slot );
}

void idD3D12DescriptorPool::CreateUav( d3d12Descriptor_t slot, ID3D12Resource & resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC & desc ) {
	device.Get().CreateUnorderedAccessView( &resource, nullptr, &desc, Cpu( slot ) );
	Mirror( slot );
}

void idD3D12DescriptorPool::CreateAccelerationStructureSrv( d3d12Descriptor_t slot, D3D12_GPU_VIRTUAL_ADDRESS address ) {
	D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
	desc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
	desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	desc.RaytracingAccelerationStructure.Location = address;
	device.Get().CreateShaderResourceView( nullptr, &desc, Cpu( slot ) );
	Mirror( slot );
}

void idD3D12DescriptorPool::Mirror( d3d12Descriptor_t slot ) {
	if ( visible ) {
		D3D12_CPU_DESCRIPTOR_HANDLE destination = visibleStart;
		destination.ptr += static_cast<SIZE_T>( slot.index ) * increment;
		device.Get().CopyDescriptorsSimple( 1, destination, Cpu( slot ), type );
	}
}

D3D12_GPU_DESCRIPTOR_HANDLE idD3D12DescriptorPool::VisibleGpu( d3d12Descriptor_t slot ) const {
	assert( visible && slot.IsValid() && slot.index < next );
	D3D12_GPU_DESCRIPTOR_HANDLE handle = visibleGpuStart;
	handle.ptr += static_cast<UINT64>( slot.index ) * increment;
	return handle;
}

idD3D12SamplerHeap::idD3D12SamplerHeap( const idD3D12Device & owner, idD3D12Graveyard & graves ) :
	device( owner ),
	graveyard( graves ),
	heap( CreateHeap( owner, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, SIZE, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, L"doom3 shader-visible samplers" ) ),
	increment( owner.Get().GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ) ) {
	if ( heap ) {
		cpuStart = heap->GetCPUDescriptorHandleForHeapStart();
	}
	fallback = Acquire( 0 );
}

uint32 idD3D12SamplerHeap::Acquire( uint64 packed ) {
	if ( const auto found = lookup.find( packed ); found != lookup.end() ) {
		slots[found->second].references++;
		return found->second;
	}
	uint32 slot = 0;
	if ( !freeSlots.empty() ) {
		slot = freeSlots.back();
		freeSlots.pop_back();
	} else if ( slots.size() < SIZE ) {
		slot = static_cast<uint32>( slots.size() );
		slots.emplace_back();
	} else {
		common->FatalError( "d3d12: more than %u samplers held at once", SIZE );
	}
	if ( heap ) {
		D3D12_CPU_DESCRIPTOR_HANDLE handle = cpuStart;
		handle.ptr += static_cast<SIZE_T>( slot ) * increment;
		const D3D12_SAMPLER_DESC desc = D3D12_SamplerFor( packed, device.Features().anisoPointMip );
		device.Get().CreateSampler( &desc, handle );
	}
	slots[slot] = { packed, 1 };
	lookup.emplace( packed, slot );
	built++;
	return slot;
}

// The open list may have read the slot: it waits in the graveyard for that list's fence.
void idD3D12SamplerHeap::Release( uint32 slot ) {
	assert( slot < slots.size() && slots[slot].references > 0 );
	if ( --slots[slot].references == 0 ) {
		lookup.erase( slots[slot].packed );
		graveyard.Bury( *this, slot );
	}
}
