// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "idlib/precompiled.h"

#include "DmapArena.h"

void dmapArena_t::Commit( size_t bytes ) {
	committed += bytes;
	if ( committed > peak ) {
		peak = committed;
	}
}

void *dmapArena_t::Alloc( size_t size ) {
	static_assert( sizeof( header_t ) == ALIGNMENT );
	static_assert( sizeof( chunk_t ) == ALIGNMENT );

	size = ( Max< size_t >( size, sizeof( freeBlock_t ) ) + ALIGNMENT - 1 ) & ~( ALIGNMENT - 1 );

	if ( size > MAX_SMALL ) {
		// links, padding to 16, header, block
		constexpr size_t LINKS = ( sizeof( largeBlock_t ) + ALIGNMENT - 1 ) & ~( ALIGNMENT - 1 );
		const size_t total = LINKS + sizeof( header_t ) + size;
		largeBlock_t * const large = static_cast< largeBlock_t * >( Mem_Alloc( total, TAG_RENDER_TOOLS ) );
		large->prev = nullptr;
		large->next = largeBlocks;
		large->size = total;
		if ( largeBlocks != nullptr ) {
			largeBlocks->prev = large;
		}
		largeBlocks = large;
		Commit( total );
		header_t * const header = reinterpret_cast< header_t * >( reinterpret_cast< byte * >( large ) + LINKS );
		header->size = size;
		header->large = large;
		return header + 1;
	}

	freeBlock_t *& freeList = freeLists[size / ALIGNMENT];
	if ( freeList != nullptr ) {
		freeBlock_t * const block = freeList;
		freeList = block->next;
		return block;
	}

	const size_t needed = sizeof( header_t ) + size;
	if ( chunks == nullptr || chunks->used + needed > CHUNK_SIZE ) {
		chunk_t * const chunk = static_cast< chunk_t * >( Mem_Alloc( CHUNK_SIZE, TAG_RENDER_TOOLS ) );
		chunk->next = chunks;
		chunk->used = sizeof( chunk_t );
		chunks = chunk;
		Commit( CHUNK_SIZE );
	}
	header_t * const header = reinterpret_cast< header_t * >( reinterpret_cast< byte * >( chunks ) + chunks->used );
	chunks->used += needed;
	header->size = size;
	header->large = nullptr;
	return header + 1;
}

void *dmapArena_t::ClearedAlloc( size_t size ) {
	void * const ptr = Alloc( size );
	memset( ptr, 0, size );
	return ptr;
}

void dmapArena_t::Free( void *ptr ) {
	if ( ptr == nullptr ) {
		return;
	}
	header_t * const header = static_cast< header_t * >( ptr ) - 1;
	if ( header->large != nullptr ) {
		largeBlock_t * const large = header->large;
		if ( large->prev != nullptr ) {
			large->prev->next = large->next;
		} else {
			largeBlocks = large->next;
		}
		if ( large->next != nullptr ) {
			large->next->prev = large->prev;
		}
		committed -= large->size;
		Mem_Free( large );
		return;
	}
	freeBlock_t * const block = static_cast< freeBlock_t * >( ptr );
	freeBlock_t *& freeList = freeLists[header->size / ALIGNMENT];
	block->next = freeList;
	freeList = block;
}

void dmapArena_t::Clear() {
	while ( chunks != nullptr ) {
		chunk_t * const next = chunks->next;
		Mem_Free( chunks );
		chunks = next;
	}
	while ( largeBlocks != nullptr ) {
		largeBlock_t * const next = largeBlocks->next;
		Mem_Free( largeBlocks );
		largeBlocks = next;
	}
	for ( freeBlock_t *& freeList : freeLists ) {
		freeList = nullptr;
	}
	committed = 0;
}

void *dmapOwnerList_t::Alloc( dmapArena_t &arena, size_t size ) {
	static_assert( sizeof( link_t ) == 16 );
	link_t * const link = static_cast< link_t * >( arena.ClearedAlloc( sizeof( link_t ) + size ) );
	link->next = head;
	if ( head != nullptr ) {
		head->prev = link;
	}
	head = link;
	return link + 1;
}

void dmapOwnerList_t::Free( dmapArena_t &arena, void *ptr ) {
	link_t * const link = static_cast< link_t * >( ptr ) - 1;
	if ( link->prev != nullptr ) {
		link->prev->next = link->next;
	} else {
		head = link->next;
	}
	if ( link->next != nullptr ) {
		link->next->prev = link->prev;
	}
	arena.Free( link );
}
