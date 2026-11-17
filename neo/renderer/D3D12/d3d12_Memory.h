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

// A committed buffer of one heap type, named for the debug tools; nullptr only when the device is lost.
[[nodiscard]] idComPtr<ID3D12Resource>	D3D12_CreateBuffer( const idD3D12Device & device, D3D12_HEAP_TYPE heap, uint64 bytes,
											D3D12_RESOURCE_STATES state, const wchar_t * name );

// CPU-written memory the GPU reads once; write-combined: write it sequentially, never read it.
struct d3d12UploadSpan_t {
	byte *							cpu;
	D3D12_GPU_VIRTUAL_ADDRESS		gpu;
	ID3D12Resource *				buffer;
	uint64							offset;			// of cpu inside buffer
};

// A persistently mapped UPLOAD buffer used as a fence-tagged ring. It never submits: a full ring is the caller's problem.
class idD3D12UploadRing {
public:
									idD3D12UploadRing( const idD3D12Device & device, uint64 size, const wchar_t * name );
	[[nodiscard]] std::optional<d3d12UploadSpan_t>	TryAllocate( uint64 bytes, uint64 align );
	void							CloseChunk( uint64 fence ) { ring.CloseChunk( fence ); }
	void							Retire( uint64 waitedFence ) { ring.Retire( waitedFence ); }
	[[nodiscard]] uint64			OldestFence() const { return ring.OldestFence(); }
	[[nodiscard]] uint64			Size() const { return ring.Size(); }

private:
	idComPtr<ID3D12Resource>		buffer;
	byte *							mapped = nullptr;
	D3D12_GPU_VIRTUAL_ADDRESS		gpuBase = 0;
	idD3D12RingAllocator			ring;
};

// The state of one tracked resource, kept in its owner's record from creation on.
struct d3d12ResourceState_t {
	D3D12_RESOURCE_STATES			current = D3D12_RESOURCE_STATE_COMMON;	// what it is in once the batch is flushed
	int16							pending = -1;	// this resource's entry in the batch
};

// Legacy transitions batched until the next draw, clear, copy, resolve or submit; a reversed pending pair cancels out.
class idD3D12Barriers {
public:
	explicit						idD3D12Barriers( idD3D12Submitter & owner ) : submitter( owner ) {}
	void							Transition( ID3D12Resource & resource, d3d12ResourceState_t & state, D3D12_RESOURCE_STATES after );
	void							Flush();			// into submitter.List(); a no-op when empty
	void							Discard();			// the list was abandoned

private:
	static constexpr int			BATCH = 64;		// a full batch flushes itself
	void							Remove( int index );

	idD3D12Submitter &				submitter;
	std::array<D3D12_RESOURCE_BARRIER, BATCH>	batch = {};
	std::array<d3d12ResourceState_t *, BATCH>	owners = {};	// owners[i] is the state of batch[i]
	int								count = 0;
};
