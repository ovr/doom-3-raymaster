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
#include "../../idlib/precompiled.h"
#include "qgl.h"

#include "../tr_local.h"
#include "gl_Backend.h"

//static const GLenum bufferUsage = GL_STATIC_DRAW_ARB;
static const GLenum bufferUsage = GL_DYNAMIC_DRAW_ARB;

/*
================================================================================================

	Buffer Objects

	Every type keeps the binding target, the map flags and the GL usage it always had:
	bufferUsage_t and the dirty range of UnmapBuffer are for backends that own the memory.

================================================================================================
*/

/*
========================
idRenderBackendGL::CreateBuffer
========================
*/
bufferHandle_t idRenderBackendGL::CreateBuffer( bufferType_t type, [[maybe_unused]] bufferUsage_t usage, int size, const void * data ) {
	if ( type == bufferType_t::JOINT ) {
		GLuint buffer = 0;
		qglGenBuffersARB( 1, &buffer );
		qglBindBufferARB( GL_UNIFORM_BUFFER, buffer );
		qglBufferDataARB( GL_UNIFORM_BUFFER, size, data, GL_STREAM_DRAW_ARB );
		qglBindBufferARB( GL_UNIFORM_BUFFER, 0);

		return GL_BufferHandle( new glBuffer_t { buffer, GL_UNIFORM_BUFFER, size } );
	}

	const bool isIndex = ( type == bufferType_t::INDEX );
	const GLenum target = isIndex ? GL_ELEMENT_ARRAY_BUFFER_ARB : GL_ARRAY_BUFFER_ARB;

	// clear out any previous error
	qglGetError();

	GLuint bufferObject = 0xFFFF;
	qglGenBuffersARB( 1, & bufferObject );
	if ( bufferObject == 0xFFFF ) {
		if ( isIndex ) {
			GLenum error = qglGetError();
			idLib::FatalError( "idRenderBackendGL::CreateBuffer: index buffer failed - GL_Error %d", error );
		}
		idLib::FatalError( "idRenderBackendGL::CreateBuffer: vertex buffer failed" );
	}
	qglBindBufferARB( target, bufferObject );

	// these are rewritten every frame
	qglBufferDataARB( target, size, data, bufferUsage );

	GLenum err = qglGetError();
	if ( err == GL_OUT_OF_MEMORY ) {
		idLib::Warning( "idRenderBackendGL::CreateBuffer: allocation of %i bytes failed", size );
		qglDeleteBuffersARB( 1, & bufferObject );
		return bufferHandle_t::NONE;
	}

	return GL_BufferHandle( new glBuffer_t { bufferObject, target, size } );
}

/*
========================
idRenderBackendGL::DestroyBuffer
========================
*/
void idRenderBackendGL::DestroyBuffer( bufferHandle_t buffer ) {
	glBuffer_t * buf = GL_Buffer( buffer );
	if ( buf == nullptr ) {
		return;
	}

	// the draw caches compare handles, and a later buffer can get this address
	if ( currentVertexBuffer == buffer ) {
		currentVertexBuffer = bufferHandle_t::NONE;
	}
	if ( currentIndexBuffer == buffer ) {
		currentIndexBuffer = bufferHandle_t::NONE;
	}

	if ( buf->target == GL_UNIFORM_BUFFER ) {
		qglBindBufferARB( GL_UNIFORM_BUFFER, 0 );
	}
	qglDeleteBuffersARB( 1, & buf->bufferObject );

	delete buf;
}

/*
========================
idRenderBackendGL::MapBuffer
========================
*/
void * idRenderBackendGL::MapBuffer( bufferHandle_t buffer ) {
	const glBuffer_t * buf = GL_Buffer( buffer );
	assert( buf != nullptr );

	qglBindBufferARB( buf->target, buf->bufferObject );
	//mapped = qglMapBufferARB( buf->target, GL_WRITE_ONLY_ARB );
	void * mapped = qglMapBufferRange( buf->target, 0, buf->size, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_UNSYNCHRONIZED_BIT );

	if ( mapped == nullptr ) {
		idLib::FatalError( "idRenderBackendGL::MapBuffer: failed" );
	}
	return mapped;
}

/*
========================
idRenderBackendGL::UnmapBuffer
========================
*/
void idRenderBackendGL::UnmapBuffer( bufferHandle_t buffer, [[maybe_unused]] int dirtyOffset, [[maybe_unused]] int dirtyBytes ) {
	const glBuffer_t * buf = GL_Buffer( buffer );
	assert( buf != nullptr );
	assert( dirtyOffset >= 0 && dirtyBytes >= 0 && dirtyOffset + dirtyBytes <= buf->size );

	qglBindBufferARB( buf->target, buf->bufferObject );
	if ( !qglUnmapBufferARB( buf->target ) ) {
		idLib::Printf( "idRenderBackendGL::UnmapBuffer failed\n" );
	}
}
