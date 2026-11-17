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
#include "d3d12_Memory.h"

idComPtr<ID3D12Resource> D3D12_CreateBuffer( const idD3D12Device & device, D3D12_HEAP_TYPE heap, uint64 bytes,
	D3D12_RESOURCE_STATES state, const wchar_t * name ) {
	D3D12_HEAP_PROPERTIES properties = {};
	properties.Type = heap;
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = bytes;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	idComPtr<ID3D12Resource> buffer;
	if ( device.Check( device.Get().CreateCommittedResource( &properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS( &buffer ) ),
			"CreateCommittedResource (buffer)" ) ) {
		buffer->SetName( name );
	}
	return buffer;
}

idD3D12UploadRing::idD3D12UploadRing( const idD3D12Device & device, uint64 size, const wchar_t * name ) :
	buffer( D3D12_CreateBuffer( device, D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_STATE_GENERIC_READ, name ) ),
	ring( size ) {
	const D3D12_RANGE nothingRead = { 0, 0 };
	if ( buffer && device.Check( buffer->Map( 0, &nothingRead, reinterpret_cast<void **>( &mapped ) ), "Map (upload ring)" ) ) {
		gpuBase = buffer->GetGPUVirtualAddress();
	}
}

std::optional<d3d12UploadSpan_t> idD3D12UploadRing::TryAllocate( uint64 bytes, uint64 align ) {
	const std::optional<uint64> offset = ring.TryAllocate( bytes, align );
	if ( !offset || mapped == nullptr ) {
		return std::nullopt;
	}
	return d3d12UploadSpan_t{ mapped + *offset, gpuBase + *offset, buffer.Get(), *offset };
}

void idD3D12Barriers::Transition( ID3D12Resource & resource, d3d12ResourceState_t & state, D3D12_RESOURCE_STATES after ) {
	if ( state.pending >= 0 ) {
		D3D12_RESOURCE_TRANSITION_BARRIER & queued = batch[state.pending].Transition;
		assert( queued.pResource == &resource );
		if ( queued.StateBefore == after ) {
			Remove( state.pending );
		} else {
			queued.StateAfter = after;
		}
		state.current = after;
		return;
	}
	if ( state.current == after ) {
		return;
	}
	if ( count == BATCH ) {
		Flush();
	}
	D3D12_RESOURCE_BARRIER & barrier = batch[count];
	barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = &resource;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = state.current;
	barrier.Transition.StateAfter = after;
	owners[count] = &state;
	state.pending = static_cast<int16>( count++ );
	state.current = after;
}

// The last entry moves into the hole, so its owner learns its new index.
void idD3D12Barriers::Remove( int index ) {
	owners[index]->pending = -1;
	count--;
	if ( index != count ) {
		batch[index] = batch[count];
		owners[index] = owners[count];
		owners[index]->pending = static_cast<int16>( index );
	}
}

void idD3D12Barriers::Flush() {
	if ( count == 0 ) {
		return;
	}
	submitter.List().ResourceBarrier( static_cast<UINT>( count ), batch.data() );
	Discard();
}

void idD3D12Barriers::Discard() {
	for ( d3d12ResourceState_t * owner : std::span( owners.data(), static_cast<size_t>( count ) ) ) {
		owner->pending = -1;
	}
	count = 0;
}
