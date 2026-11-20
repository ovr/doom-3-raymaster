// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#ifndef __DMAPARENA_H__
#define __DMAPARENA_H__

/*
===============================================================================

	The memory of one compile.

	Blocks are cut from large chunks. A freed block goes on the list of its
	size and is handed out again, so the triangle churn of the t-junction
	fixer and the optimizer stays bounded. Clear() returns everything to the
	heap at once, whatever was freed: a compile that ends with an error
	leaves nothing behind.

	  chunk:  | header | block | header | block | ...  free ... |
	  header: the block's size, or the links of a block too large for a chunk

	Not thread safe: a compile runs on one thread.

===============================================================================
*/

class dmapArena_t {
public:
						dmapArena_t() = default;
						~dmapArena_t() { Clear(); }
						dmapArena_t( const dmapArena_t & ) = delete;
	dmapArena_t &		operator=( const dmapArena_t & ) = delete;

						// 16 byte aligned, not initialized
	[[nodiscard]] void *Alloc( size_t size );
	[[nodiscard]] void *ClearedAlloc( size_t size );
	void				Free( void *ptr );
	void				Clear();

						// bytes taken from the heap now, and the most since the last ResetPeak()
	[[nodiscard]] size_t Committed() const { return committed; }
	[[nodiscard]] size_t Peak() const { return peak; }
	void				ResetPeak() { peak = committed; }

	template< typename T >
	[[nodiscard]] T *	AllocType( int count = 1 ) { return static_cast< T * >( Alloc( sizeof( T ) * count ) ); }
	template< typename T >
	[[nodiscard]] T *	ClearedAllocType( int count = 1 ) { return static_cast< T * >( ClearedAlloc( sizeof( T ) * count ) ); }

private:
	static constexpr size_t ALIGNMENT = 16;
	static constexpr size_t CHUNK_SIZE = 4 * 1024 * 1024;
	static constexpr size_t MAX_SMALL = 4096;				// larger blocks come from the heap one by one
	static constexpr size_t NUM_FREE_LISTS = MAX_SMALL / ALIGNMENT + 1;

	struct chunk_t {
		chunk_t *		next;
		size_t			used;
	};
	struct freeBlock_t {
		freeBlock_t *	next;
	};
	struct largeBlock_t {
		largeBlock_t *	prev;
		largeBlock_t *	next;
		size_t			size;
	};
	// in front of every block; 16 bytes, so the block keeps the alignment of the header
	struct header_t {
		size_t			size;			// rounded up; above MAX_SMALL the block is a largeBlock_t
		largeBlock_t *	large;
	};

	chunk_t *			chunks = nullptr;
	largeBlock_t *		largeBlocks = nullptr;
	freeBlock_t *		freeLists[NUM_FREE_LISTS] = {};
	size_t				committed = 0;
	size_t				peak = 0;

	void				Commit( size_t bytes );
};

/*
===============================================================================

	The arena objects of one kind that own something of the heap: brushes,
	faces and portals hold idWinding pointers. Each is linked through a pair
	of pointers in front of it, so that what is still alive when a compile
	ends, whichever way, can be walked and its windings deleted.

===============================================================================
*/

class dmapOwnerList_t {
public:
						// cleared
	[[nodiscard]] void *Alloc( dmapArena_t &arena, size_t size );
	void				Free( dmapArena_t &arena, void *ptr );
						// forgets the objects: their memory goes with the arena
	void				Reset() { head = nullptr; }

	[[nodiscard]] void *First() const { return ( head != nullptr ) ? head + 1 : nullptr; }
	[[nodiscard]] static void *Next( void *ptr ) {
		const link_t * const next = ( static_cast< link_t * >( ptr ) - 1 )->next;
		return ( next != nullptr ) ? const_cast< link_t * >( next ) + 1 : nullptr;
	}

private:
	struct link_t {
		link_t *		prev;
		link_t *		next;
	};
	link_t *			head = nullptr;
};

#endif /* !__DMAPARENA_H__ */
