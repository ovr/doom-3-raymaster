/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company. 

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
#include "../idlib/precompiled.h"

#include <new>

//===============================================================
//
//	memory allocation all in one place
//
//===============================================================

#undef new

namespace {

// every block is at least this aligned and its size is padded to a multiple of it
constexpr size_t MEM_ALIGNMENT = 16;

/*
==================
AlignedAlloc

nullptr when out of memory, also when the padded size doesn't fit in a size_t:
new[] asks for SIZE_MAX when the array size overflows, and that must fail
instead of wrapping around to a tiny block.
==================
*/
void * AlignedAlloc( const size_t size, const size_t alignment ) noexcept {
	if ( size > SIZE_MAX - ( MEM_ALIGNMENT - 1 ) ) {
		return nullptr;
	}
	const size_t paddedSize = ( size + MEM_ALIGNMENT - 1 ) & ~( MEM_ALIGNMENT - 1 );
	return _aligned_malloc( paddedSize, Max( alignment, MEM_ALIGNMENT ) );
}

/*
==================
NewAlloc

What a throwing operator new must do: return a non-null pointer, a distinct one
for zero bytes too, or call the new handler until it frees enough memory and
throw std::bad_alloc when there is none.
==================
*/
void * NewAlloc( const size_t size, const size_t alignment ) {
	const size_t requested = ( size != 0 ) ? size : 1;
	for ( ;; ) {
		if ( void * const ptr = AlignedAlloc( requested, alignment ) ) {
			return ptr;
		}
		const std::new_handler handler = std::get_new_handler();
		if ( handler == nullptr ) {
			throw std::bad_alloc();
		}
		handler();
	}
}

/*
==================
NewAllocNoThrow

The nothrow operator new: the throwing one, nullptr instead of the exception.
==================
*/
void * NewAllocNoThrow( const size_t size, const size_t alignment ) noexcept {
	try {
		return NewAlloc( size, alignment );
	} catch ( ... ) {
		return nullptr;
	}
}

}	// namespace

/*
==================
Mem_Alloc16
==================
*/
void * Mem_Alloc16( const int size, const memTag_t tag ) {
	if ( size <= 0 ) {
		return nullptr;
	}
	return AlignedAlloc( static_cast<size_t>( size ), MEM_ALIGNMENT );
}

/*
==================
Mem_Free16
==================
*/
void Mem_Free16( void *ptr ) {
	if ( ptr == nullptr ) {
		return;
	}
	_aligned_free( ptr );
}

/*
===============================================================================
The replaceable global operator new and delete, see Heap.h. The std::align_val_t forms
(types aligned to more than 8 on x86) raise the alignment to the type's.
===============================================================================
*/

void * operator new( size_t size ) {
	return NewAlloc( size, MEM_ALIGNMENT );
}
void * operator new[]( size_t size ) {
	return NewAlloc( size, MEM_ALIGNMENT );
}
void * operator new( size_t size, const std::nothrow_t & ) noexcept {
	return NewAllocNoThrow( size, MEM_ALIGNMENT );
}
void * operator new[]( size_t size, const std::nothrow_t & ) noexcept {
	return NewAllocNoThrow( size, MEM_ALIGNMENT );
}
void * operator new( size_t size, std::align_val_t alignment ) {
	return NewAlloc( size, static_cast<size_t>( alignment ) );
}
void * operator new[]( size_t size, std::align_val_t alignment ) {
	return NewAlloc( size, static_cast<size_t>( alignment ) );
}
void * operator new( size_t size, std::align_val_t alignment, const std::nothrow_t & ) noexcept {
	return NewAllocNoThrow( size, static_cast<size_t>( alignment ) );
}
void * operator new[]( size_t size, std::align_val_t alignment, const std::nothrow_t & ) noexcept {
	return NewAllocNoThrow( size, static_cast<size_t>( alignment ) );
}

// _aligned_free doesn't need the size or the alignment
void operator delete( void * ptr ) noexcept {
	Mem_Free16( ptr );
}
void operator delete[]( void * ptr ) noexcept {
	Mem_Free16( ptr );
}
void operator delete( void * ptr, size_t ) noexcept {
	Mem_Free16( ptr );
}
void operator delete[]( void * ptr, size_t ) noexcept {
	Mem_Free16( ptr );
}
void operator delete( void * ptr, const std::nothrow_t & ) noexcept {
	Mem_Free16( ptr );
}
void operator delete[]( void * ptr, const std::nothrow_t & ) noexcept {
	Mem_Free16( ptr );
}
void operator delete( void * ptr, std::align_val_t ) noexcept {
	Mem_Free16( ptr );
}
void operator delete[]( void * ptr, std::align_val_t ) noexcept {
	Mem_Free16( ptr );
}
void operator delete( void * ptr, size_t, std::align_val_t ) noexcept {
	Mem_Free16( ptr );
}
void operator delete[]( void * ptr, size_t, std::align_val_t ) noexcept {
	Mem_Free16( ptr );
}
void operator delete( void * ptr, std::align_val_t, const std::nothrow_t & ) noexcept {
	Mem_Free16( ptr );
}
void operator delete[]( void * ptr, std::align_val_t, const std::nothrow_t & ) noexcept {
	Mem_Free16( ptr );
}

/*
==================
new ( TAG_X ) T

Placement forms, not replacements. They follow the rules of the throwing
operator new; Mem_Alloc16 doesn't track tags yet, so the tag is unused.
==================
*/
void * operator new( size_t size, memTag_t ) {
	return NewAlloc( size, MEM_ALIGNMENT );
}
void * operator new[]( size_t size, memTag_t ) {
	return NewAlloc( size, MEM_ALIGNMENT );
}
void operator delete( void * ptr, memTag_t ) noexcept {
	Mem_Free16( ptr );
}
void operator delete[]( void * ptr, memTag_t ) noexcept {
	Mem_Free16( ptr );
}

/*
==================
Mem_ClearedAlloc
==================
*/
void * Mem_ClearedAlloc( const int size, const memTag_t tag ) {
	void * mem = Mem_Alloc( size, tag );
	SIMDProcessor->Memset( mem, 0, size );
	return mem;
}

/*
==================
Mem_CopyString
==================
*/
char *Mem_CopyString( const char *in ) {
	char * out = (char *)Mem_Alloc( strlen(in) + 1, TAG_STRING );
	strcpy( out, in );
	return out;
}

