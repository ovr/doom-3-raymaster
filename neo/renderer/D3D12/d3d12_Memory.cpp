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

namespace {

// CopyBufferRegion needs no alignment; the vertex cache's 16 keeps the copies of its ranges aligned.
constexpr uint64 BUFFER_COPY_ALIGN = 16;

}	// namespace

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

// A decaying resource starts every list in COMMON; a pending barrier never outlives its list (SubmitChunk flushes).
void idD3D12Barriers::Transition( ID3D12Resource & resource, d3d12ResourceState_t & state, D3D12_RESOURCE_STATES after ) {
	if ( state.decays && state.listSerial != submitter.ListSerial() ) {
		assert( state.pending < 0 );
		state.current = D3D12_RESOURCE_STATE_COMMON;
		state.listSerial = submitter.ListSerial();
	}
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
	issued += count;
	Discard();
}

void idD3D12Barriers::Discard() {
	for ( d3d12ResourceState_t * owner : std::span( owners.data(), static_cast<size_t>( count ) ) ) {
		owner->pending = -1;
	}
	count = 0;
}

idD3D12Uploader::idD3D12Uploader( const idD3D12Device & owner, idD3D12Submitter & submit, idD3D12UploadRing & ring, idD3D12Barriers & batch,
	idD3D12Graveyard & graves ) :
	device( owner ),
	submitter( submit ),
	staging( ring ),
	barriers( batch ),
	graveyard( graves ) {
	assert( staging.Size() >= PIECE );
}

std::optional<d3d12UploadSpan_t> idD3D12Uploader::Stage( uint64 bytes, uint64 align ) {
	bytesThisFrame += bytes;
	if ( bytes <= PIECE ) {
		return Room( bytes, align );
	}
	// only the biggest texture levels: simpler than cutting compressed rows into ring pieces
	idComPtr<ID3D12Resource> buffer = D3D12_CreateBuffer( device, D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ,
		L"doom3 staging (one large copy)" );
	const D3D12_RANGE nothingRead = { 0, 0 };
	void * mapped = nullptr;
	if ( !buffer || !device.Check( buffer->Map( 0, &nothingRead, &mapped ), "Map (large staging buffer)" ) ) {
		return std::nullopt;
	}
	const d3d12UploadSpan_t span = { static_cast<byte *>( mapped ), buffer->GetGPUVirtualAddress(), buffer.Get(), 0 };
	graveyard.Bury( std::move( buffer ) );	// tagged with the open list, which records the copy
	return span;
}

d3d12UploadSpan_t idD3D12Uploader::Room( uint64 bytes, uint64 align ) {
	for ( ;; ) {
		if ( const std::optional<d3d12UploadSpan_t> span = staging.TryAllocate( bytes, align ) ) {
			return *span;
		}
		submitter.SubmitChunk();
		const uint64 oldest = staging.OldestFence();
		if ( oldest == 0 ) {
			common->FatalError( "d3d12: %llu bytes do not fit into the empty staging ring", static_cast<unsigned long long>( bytes ) );
		}
		submitter.WaitFor( oldest );
	}
}

void idD3D12Uploader::Pace( uint64 bytes ) {
	if ( paceList != submitter.ListSerial() ) {
		paceList = submitter.ListSerial();
		bytesInList = 0;
	}
	bytesInList += bytes;
	if ( bytesInList >= PIECE ) {
		submitter.SubmitChunk();
	}
}

// A new list after a submit finds the buffer back in COMMON, so each piece transitions again.
void idD3D12Uploader::BufferRange( ID3D12Resource & buffer, d3d12ResourceState_t & state, D3D12_RESOURCE_STATES useState,
	uint64 offset, std::span<const byte> bytes ) {
	for ( size_t done = 0; done < bytes.size(); ) {
		const size_t piece = Min( bytes.size() - done, static_cast<size_t>( PIECE ) );
		const std::optional<d3d12UploadSpan_t> span = Stage( piece, BUFFER_COPY_ALIGN );
		if ( !span ) {
			return;
		}
		memcpy( span->cpu, bytes.data() + done, piece );
		barriers.Transition( buffer, state, D3D12_RESOURCE_STATE_COPY_DEST );
		barriers.Flush();
		submitter.List().CopyBufferRegion( &buffer, offset + done, span->buffer, span->offset, piece );
		done += piece;
		if ( done == bytes.size() ) {
			barriers.Transition( buffer, state, useState );
		}
		Pace( piece );
	}
}

// Consecutive copies into a texture need no barrier between them: it stays in COPY_DEST until a bind.
void idD3D12Uploader::TextureRegion( ID3D12Resource & texture, d3d12ResourceState_t & state, UINT subresource, const d3d12UploadSpan_t & span,
	const D3D12_SUBRESOURCE_FOOTPRINT & footprint, UINT rows ) {
	barriers.Transition( texture, state, D3D12_RESOURCE_STATE_COPY_DEST );
	barriers.Flush();
	D3D12_TEXTURE_COPY_LOCATION destination = {};
	destination.pResource = &texture;
	destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	destination.SubresourceIndex = subresource;
	D3D12_TEXTURE_COPY_LOCATION source = {};
	source.pResource = span.buffer;
	source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	source.PlacedFootprint.Offset = span.offset;
	source.PlacedFootprint.Footprint = footprint;
	submitter.List().CopyTextureRegion( &destination, 0, 0, 0, &source, nullptr );
	Pace( static_cast<uint64>( footprint.RowPitch ) * rows );
}
