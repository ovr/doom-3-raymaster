// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

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
