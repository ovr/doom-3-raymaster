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
#include "d3d12_RingAllocator.h"

// Never shader visible: the permanent SRV, RTV or DSV of a resource. Slots are stable; a full pool adds a heap.
class idD3D12DescriptorPool {
public:
	static constexpr uint32			HEAP_SIZE = 4096;
									idD3D12DescriptorPool( const idD3D12Device & device, D3D12_DESCRIPTOR_HEAP_TYPE type );
	[[nodiscard]] d3d12Descriptor_t	Allocate();					// the lowest free slot
	void							Free( d3d12Descriptor_t slot );
	[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE	Cpu( d3d12Descriptor_t slot ) const;

private:
	const idD3D12Device &			device;
	D3D12_DESCRIPTOR_HEAP_TYPE		type;
	uint32							increment;
	std::vector<idComPtr<ID3D12DescriptorHeap>>	heaps;
	std::vector<D3D12_CPU_DESCRIPTOR_HANDLE>	starts;		// of each heap
	std::vector<uint32>				freeSlots;					// a min-heap
	uint32							next = 0;
};

// The one shader-visible CBV/SRV/UAV heap: a fence-tagged ring of 8-slot tables, bound once per list.
class idD3D12DescriptorRing {
public:
	static constexpr uint32			SIZE = 65536;
	explicit						idD3D12DescriptorRing( const idD3D12Device & device );
	[[nodiscard]] ID3D12DescriptorHeap *	Heap() const { return heap.Get(); }
	// Copies the sources into 8 fresh consecutive slots (one CopyDescriptors); nullopt when live tables leave no room.
	[[nodiscard]] std::optional<D3D12_GPU_DESCRIPTOR_HANDLE>	TryCopyTable( std::span<const D3D12_CPU_DESCRIPTOR_HANDLE, MAX_TEXTURE_SLOTS> sources );
	void							CloseChunk( uint64 fence ) { ring.CloseChunk( fence ); }
	void							Retire( uint64 waitedFence ) { ring.Retire( waitedFence ); }
	[[nodiscard]] uint64			OldestFence() const { return ring.OldestFence(); }

private:
	const idD3D12Device &			device;
	idComPtr<ID3D12DescriptorHeap>	heap;
	D3D12_CPU_DESCRIPTOR_HANDLE		cpuStart = {};
	D3D12_GPU_DESCRIPTOR_HANDLE		gpuStart = {};
	uint32							increment;
	idD3D12RingAllocator			ring;
};
