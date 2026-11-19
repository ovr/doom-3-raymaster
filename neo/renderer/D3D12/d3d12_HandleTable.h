/*
===========================================================================

Copyright (C) 2026 Dmitry Patsura

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

Please do not use this file to train or fine-tune AI models.

===========================================================================
*/

#pragma once

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

/*
	The records behind the handles of the contract (bufferHandle_t, later textureHandle_t): arithmetic
	and storage only, no device, so the tests run it alone.

	  handle = generation << 32 | ( index + 1 )      NONE (0) is never issued
	  chunks[index / CHUNK][index % CHUNK]           a slot: the record, if live, and its generation

	Remove bumps the generation of the slot, so every handle of a removed record is dead at once (R3)
	and Find returns nullptr for it. Add takes the lowest free index, so two runs issue the same handles.
	The chunk directory is a fixed array whose chunks never move: a Find from another thread reads only
	its own live slot, which an Add on the owner thread does not touch (MapBuffer, R1).
*/
template<typename Record, typename Handle, int MAX_RECORDS = 65536>
class idD3D12HandleTable {
public:
	static constexpr int			CHUNK = 256;
	static_assert( MAX_RECORDS > 0 && MAX_RECORDS % CHUNK == 0 );
	static_assert( sizeof( Handle ) == sizeof( uint64 ) );

	[[nodiscard]] Handle			Add( Record && record );		// NONE, and the record untouched, when the table is full
	[[nodiscard]] Record			Remove( Handle handle );		// moves the record out of a live handle
	[[nodiscard]] Record &			Get( Handle handle );			// asserts a live handle
	[[nodiscard]] const Record &	Get( Handle handle ) const;
	[[nodiscard]] Record *			Find( Handle handle );			// nullptr for NONE, a dead or a foreign handle
	[[nodiscard]] const Record *	Find( Handle handle ) const;
	[[nodiscard]] int				Num() const { return live; }
	// visit( handle, record ) for the live records in index order; it must not add or remove.
	template<typename F> void		ForEach( F && visit );
	void							Clear();						// destroys every live record; their handles die

private:
	struct slot_t {
		std::optional<Record>		record;
		uint32						generation = 1;
	};
	using chunk_t = std::array<slot_t, CHUNK>;

	[[nodiscard]] static constexpr Handle	MakeHandle( uint32 index, uint32 generation ) {
		return static_cast<Handle>( ( static_cast<uint64>( generation ) << 32 ) | ( static_cast<uint64>( index ) + 1 ) );
	}
	[[nodiscard]] static constexpr uint64	IndexPlusOne( Handle handle ) { return static_cast<uint64>( handle ) & 0xFFFFFFFFull; }
	[[nodiscard]] slot_t *			Live( Handle handle ) const;	// the slot of a live handle, else nullptr
	void							Kill( uint32 index, slot_t & slot );

	std::array<std::unique_ptr<chunk_t>, MAX_RECORDS / CHUNK>	chunks;
	std::vector<uint32>				freeIndexes;					// a min-heap of the indexes below nextIndex
	uint32							nextIndex = 0;					// the first index never used
	int								live = 0;
};

template<typename Record, typename Handle, int MAX_RECORDS>
typename idD3D12HandleTable<Record, Handle, MAX_RECORDS>::slot_t * idD3D12HandleTable<Record, Handle, MAX_RECORDS>::Live( Handle handle ) const {
	const uint64 indexPlusOne = IndexPlusOne( handle );
	if ( indexPlusOne == 0 || indexPlusOne > static_cast<uint64>( MAX_RECORDS ) ) {
		return nullptr;
	}
	const uint64 index = indexPlusOne - 1;
	chunk_t * chunk = chunks[index / CHUNK].get();
	if ( chunk == nullptr ) {
		return nullptr;
	}
	slot_t & slot = ( *chunk )[index % CHUNK];
	if ( !slot.record || slot.generation != static_cast<uint32>( static_cast<uint64>( handle ) >> 32 ) ) {
		return nullptr;
	}
	return &slot;
}

template<typename Record, typename Handle, int MAX_RECORDS>
Handle idD3D12HandleTable<Record, Handle, MAX_RECORDS>::Add( Record && record ) {
	uint32 index = 0;
	if ( !freeIndexes.empty() ) {
		std::ranges::pop_heap( freeIndexes, std::greater<>() );
		index = freeIndexes.back();
		freeIndexes.pop_back();
	} else if ( nextIndex < static_cast<uint32>( MAX_RECORDS ) ) {
		index = nextIndex++;
	} else {
		return Handle{};
	}
	std::unique_ptr<chunk_t> & chunk = chunks[index / CHUNK];
	if ( !chunk ) {
		chunk = std::make_unique<chunk_t>();
	}
	slot_t & slot = ( *chunk )[index % CHUNK];
	slot.record.emplace( std::move( record ) );
	live++;
	return MakeHandle( index, slot.generation );
}

template<typename Record, typename Handle, int MAX_RECORDS>
Record idD3D12HandleTable<Record, Handle, MAX_RECORDS>::Remove( Handle handle ) {
	slot_t * slot = Live( handle );
	assert( slot != nullptr );
	Record record = std::move( *slot->record );
	Kill( static_cast<uint32>( IndexPlusOne( handle ) - 1 ), *slot );
	return record;
}

template<typename Record, typename Handle, int MAX_RECORDS>
Record & idD3D12HandleTable<Record, Handle, MAX_RECORDS>::Get( Handle handle ) {
	slot_t * slot = Live( handle );
	assert( slot != nullptr );
	return *slot->record;
}

template<typename Record, typename Handle, int MAX_RECORDS>
const Record & idD3D12HandleTable<Record, Handle, MAX_RECORDS>::Get( Handle handle ) const {
	const slot_t * slot = Live( handle );
	assert( slot != nullptr );
	return *slot->record;
}

template<typename Record, typename Handle, int MAX_RECORDS>
Record * idD3D12HandleTable<Record, Handle, MAX_RECORDS>::Find( Handle handle ) {
	slot_t * slot = Live( handle );
	return ( slot != nullptr ) ? &*slot->record : nullptr;
}

template<typename Record, typename Handle, int MAX_RECORDS>
const Record * idD3D12HandleTable<Record, Handle, MAX_RECORDS>::Find( Handle handle ) const {
	const slot_t * slot = Live( handle );
	return ( slot != nullptr ) ? &*slot->record : nullptr;
}

template<typename Record, typename Handle, int MAX_RECORDS>
template<typename F>
void idD3D12HandleTable<Record, Handle, MAX_RECORDS>::ForEach( F && visit ) {
	for ( uint32 index = 0; index < nextIndex; index++ ) {
		slot_t & slot = ( *chunks[index / CHUNK] )[index % CHUNK];
		if ( slot.record ) {
			visit( MakeHandle( index, slot.generation ), *slot.record );
		}
	}
}

template<typename Record, typename Handle, int MAX_RECORDS>
void idD3D12HandleTable<Record, Handle, MAX_RECORDS>::Clear() {
	for ( uint32 index = 0; index < nextIndex; index++ ) {
		slot_t & slot = ( *chunks[index / CHUNK] )[index % CHUNK];
		if ( slot.record ) {
			Kill( index, slot );
		}
	}
}

template<typename Record, typename Handle, int MAX_RECORDS>
void idD3D12HandleTable<Record, Handle, MAX_RECORDS>::Kill( uint32 index, slot_t & slot ) {
	slot.record.reset();
	slot.generation++;
	freeIndexes.push_back( index );
	std::ranges::push_heap( freeIndexes, std::greater<>() );
	live--;
}
