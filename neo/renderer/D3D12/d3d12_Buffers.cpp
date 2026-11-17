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
#include "d3d12_Buffers.h"

namespace {

constexpr float DEFAULT_ATTRIB[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

// The state a staged buffer is left in after a copy: what its draws read it as.
constexpr D3D12_RESOURCE_STATES UseState( bufferType_t type ) {
	return ( type == bufferType_t::INDEX ) ? D3D12_RESOURCE_STATE_INDEX_BUFFER : D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
}

// For PIX and the debug layer's messages.
const wchar_t * BufferName( bufferType_t type, bufferUsage_t usage ) {
	constexpr const wchar_t * NAMES[3][2] = {
		{ L"doom3 STATIC vertex buffer", L"doom3 FRAME vertex buffer" },
		{ L"doom3 STATIC index buffer", L"doom3 FRAME index buffer" },
		{ L"doom3 STATIC joint buffer", L"doom3 FRAME joint buffer" },
	};
	return NAMES[static_cast<size_t>( type )][static_cast<size_t>( usage )];
}

// A persistently mapped UPLOAD buffer; nullptr on a lost device.
byte * MapUpload( const idD3D12Device & device, ID3D12Resource & buffer ) {
	const D3D12_RANGE nothingRead = { 0, 0 };
	void * mapped = nullptr;
	return device.Check( buffer.Map( 0, &nothingRead, &mapped ), "Map (upload buffer)" ) ? static_cast<byte *>( mapped ) : nullptr;
}

}	// namespace

idD3D12BufferStore::idD3D12BufferStore( const idD3D12Device & owner, idD3D12Graveyard & graves, idD3D12Uploader & copies,
	idD3D12Barriers & batch, d3d12BufferPlacement_t frames ) :
	device( owner ),
	graveyard( graves ),
	uploader( copies ),
	barriers( batch ),
	framePlacement( frames ) {
}

// Both live in the UPLOAD heap: GENERIC_READ never changes, so no draw has to transition them in its list.
void idD3D12BufferStore::CreateDefaults() {
	zeroJoints = D3D12_CreateBuffer( device, D3D12_HEAP_TYPE_UPLOAD, JOINT_TAIL, D3D12_RESOURCE_STATE_GENERIC_READ, L"doom3 zero joints" );
	defaultAttribs = D3D12_CreateBuffer( device, D3D12_HEAP_TYPE_UPLOAD, sizeof( DEFAULT_ATTRIB ), D3D12_RESOURCE_STATE_GENERIC_READ,
		L"doom3 default attribute" );
	if ( !zeroJoints || !defaultAttribs ) {
		return;
	}
	if ( byte * joints = MapUpload( device, *zeroJoints.Get() ) ) {
		memset( joints, 0, JOINT_TAIL );
		zeroJoints->Unmap( 0, nullptr );
	}
	if ( byte * attrib = MapUpload( device, *defaultAttribs.Get() ) ) {
		memcpy( attrib, DEFAULT_ATTRIB, sizeof( DEFAULT_ATTRIB ) );
		defaultAttribs->Unmap( 0, nullptr );
	}
}

// NONE on a lost device or a full table, which the vertex cache reports. Committed resources start zeroed.
bufferHandle_t idD3D12BufferStore::Create( bufferType_t type, bufferUsage_t usage, int size, const void * data ) {
	assert( size > 0 );
	d3d12Buffer_t buffer;
	buffer.type = type;
	buffer.usage = usage;
	buffer.placement = ( usage == bufferUsage_t::FRAME ) ? framePlacement : d3d12BufferPlacement_t::STAGED;
	buffer.size = size;
	buffer.bytes = static_cast<uint64>( size ) + ( ( type == bufferType_t::JOINT ) ? JOINT_TAIL : 0 );
	const bool staged = ( buffer.placement == d3d12BufferPlacement_t::STAGED );
	buffer.resource = D3D12_CreateBuffer( device, staged ? D3D12_HEAP_TYPE_DEFAULT : D3D12_HEAP_TYPE_UPLOAD, buffer.bytes,
		staged ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_GENERIC_READ, BufferName( type, usage ) );
	if ( !buffer.resource ) {
		return bufferHandle_t::NONE;
	}
	buffer.gpu = buffer.resource->GetGPUVirtualAddress();
	if ( staged ) {
		buffer.mirror.reset( static_cast<byte *>( Mem_ClearedAlloc( static_cast<size_t>( size ), TAG_RENDER_STATIC ) ) );
		buffer.cpu = buffer.mirror.get();
		buffer.state.decays = true;
	} else {
		buffer.cpu = MapUpload( device, *buffer.resource.Get() );
		if ( buffer.cpu == nullptr ) {
			return bufferHandle_t::NONE;
		}
	}
	if ( data != nullptr ) {
		memcpy( buffer.cpu, data, static_cast<size_t>( size ) );
	}

	const uint64 resourceBytes = buffer.bytes;
	const bufferHandle_t handle = table.Add( std::move( buffer ) );
	if ( handle == bufferHandle_t::NONE ) {
		common->Warning( "d3d12: no room for buffer %d in the handle table", table.Num() + 1 );
		return bufferHandle_t::NONE;
	}
	bytes[static_cast<size_t>( usage )] += resourceBytes;
	mirrorBytes += staged ? static_cast<uint64>( size ) : 0;
	if ( staged && data != nullptr ) {
		Unmap( handle, 0, size );
	}
	return handle;
}

// The memory dies after the fence of the open list (R3). A batched barrier points at the state inside the
// record, so it goes into the list before the record moves.
void idD3D12BufferStore::Destroy( bufferHandle_t handle ) {
	const d3d12Buffer_t * buffer = table.Find( handle );
	if ( buffer == nullptr ) {
		return;
	}
	if ( buffer->state.pending >= 0 ) {
		barriers.Flush();
	}
	d3d12Buffer_t dead = table.Remove( handle );
	bytes[static_cast<size_t>( dead.usage )] -= dead.bytes;
	mirrorBytes -= dead.mirror ? static_cast<uint64>( dead.size ) : 0;
	graveyard.Bury( std::move( dead.resource ) );
}

void * idD3D12BufferStore::Map( bufferHandle_t handle ) const {
	const d3d12Buffer_t * buffer = table.Find( handle );
	assert( buffer != nullptr );
	return ( buffer != nullptr ) ? buffer->cpu : nullptr;
}

// An UPLOAD buffer was written in place; a staged one copies the dirty range of its mirror.
void idD3D12BufferStore::Unmap( bufferHandle_t handle, int dirtyOffset, int dirtyBytes ) {
	d3d12Buffer_t * buffer = table.Find( handle );
	if ( buffer == nullptr || dirtyBytes <= 0 ) {
		return;
	}
	assert( dirtyOffset >= 0 && dirtyOffset + dirtyBytes <= buffer->size );
	if ( buffer->placement != d3d12BufferPlacement_t::STAGED ) {
		inPlaceBytes += static_cast<uint64>( dirtyBytes );
		return;
	}
	const std::span<const byte> dirty( buffer->cpu + dirtyOffset, static_cast<size_t>( dirtyBytes ) );
	uploader.BufferRange( *buffer->resource.Get(), buffer->state, UseState( buffer->type ), static_cast<uint64>( dirtyOffset ), dirty );
}

const d3d12Buffer_t * idD3D12BufferStore::Use( bufferHandle_t handle ) {
	d3d12Buffer_t * buffer = table.Find( handle );
	if ( buffer != nullptr && buffer->placement == d3d12BufferPlacement_t::STAGED ) {
		barriers.Transition( *buffer->resource.Get(), buffer->state, UseState( buffer->type ) );
	}
	return buffer;
}

D3D12_GPU_VIRTUAL_ADDRESS idD3D12BufferStore::ZeroJoints() const {
	return zeroJoints ? zeroJoints->GetGPUVirtualAddress() : 0;
}

D3D12_VERTEX_BUFFER_VIEW idD3D12BufferStore::DefaultAttribs() const {
	return { defaultAttribs ? defaultAttribs->GetGPUVirtualAddress() : 0, sizeof( DEFAULT_ATTRIB ), 0 };
}

// Shared code frees its buffers first (the vertex cache shuts down before the backend); this is the rest.
void idD3D12BufferStore::DestroyAll() {
	if ( table.Num() > 0 ) {
		common->DPrintf( "d3d12: %d buffers still alive at shutdown\n", table.Num() );
	}
	table.Clear();
	bytes = {};
	mirrorBytes = 0;
}
