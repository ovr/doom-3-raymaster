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
#ifndef __BUFFEROBJECT_H__
#define __BUFFEROBJECT_H__

#include "RenderBackendTypes.h"

/*
================================================================================================

	Buffer Objects

	A buffer of the render backend and its size. The backend is called on its owner thread
	only; the memory MapBuffer returns may be written by any thread until UnmapBuffer.

================================================================================================
*/

class idBufferObject {
public:
	// Allocate or free the buffer. data may be nullptr.
	bool				AllocBufferObject( const void * data, int allocSize, bufferUsage_t usage );
	void				FreeBufferObject();

	// Maps the whole buffer for writing only.
	void *				MapBuffer();
	// Only [dirtyOffset, dirtyOffset + dirtyBytes) was written since MapBuffer.
	void				UnmapBuffer( int dirtyOffset, int dirtyBytes );

	bufferHandle_t		GetHandle() const { return handle; }
	int					GetSize() const { return size; }

protected:
	explicit			idBufferObject( bufferType_t bufferType ) : type( bufferType ) {}

private:
	bufferHandle_t		handle = bufferHandle_t::NONE;
	int					size = 0;				// size in bytes
	const bufferType_t	type;

	DISALLOW_COPY_AND_ASSIGN( idBufferObject );
};

class idVertexBuffer : public idBufferObject {
public:
						idVertexBuffer() : idBufferObject( bufferType_t::VERTEX ) {}
};

class idIndexBuffer : public idBufferObject {
public:
						idIndexBuffer() : idBufferObject( bufferType_t::INDEX ) {}
};

// Draws bind a range of it at an offset that is a multiple of Caps().uniformBufferOffsetAlignment,
// at most 256 bytes: joint offsets, multiples of 48 bytes, come in steps of 16 joints = 768 bytes.
class idJointBuffer : public idBufferObject {
public:
						idJointBuffer() : idBufferObject( bufferType_t::JOINT ) {}
};

#endif // !__BUFFEROBJECT_H__
