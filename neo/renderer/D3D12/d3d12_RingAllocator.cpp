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

#include "d3d12_RingAllocator.h"

idD3D12RingAllocator::idD3D12RingAllocator( uint64 ringSize ) :
	size( ringSize ) {
	assert( size > 0 );
}

std::optional<uint64> idD3D12RingAllocator::TryAllocate( uint64 bytes, uint64 align ) {
	assert( bytes > 0 && align > 0 && ( align & ( align - 1 ) ) == 0 );
	if ( used == size || bytes > size ) {
		return std::nullopt;
	}
	if ( used == 0 ) {
		head = tail = 0;		// an empty ring starts over: its whole length is one free run
	}
	const uint64 aligned = ( head + align - 1 ) & ~( align - 1 );
	uint64 offset = aligned;
	uint64 consumed = 0;
	if ( head >= tail ) {
		// free: [head, size) and [0, tail)
		if ( aligned + bytes <= size ) {
			consumed = aligned + bytes - head;
		} else if ( bytes <= tail ) {
			offset = 0;
			consumed = ( size - head ) + bytes;
		} else {
			return std::nullopt;
		}
	} else {
		// free: [head, tail)
		if ( aligned + bytes > tail ) {
			return std::nullopt;
		}
		consumed = aligned + bytes - head;
	}
	head = ( offset + bytes == size ) ? 0 : offset + bytes;
	used += consumed;
	open += consumed;
	return offset;
}

void idD3D12RingAllocator::CloseChunk( uint64 fence ) {
	if ( open == 0 ) {
		return;
	}
	assert( chunks.empty() || chunks.back().fence <= fence );
	chunks.push_back( { head, open, fence } );
	open = 0;
}

void idD3D12RingAllocator::Retire( uint64 waitedFence ) {
	while ( !chunks.empty() && chunks.front().fence <= waitedFence ) {
		tail = chunks.front().end;
		used -= chunks.front().bytes;
		chunks.pop_front();
	}
}

uint64 idD3D12RingAllocator::OldestFence() const {
	return chunks.empty() ? 0 : chunks.front().fence;
}
