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

/*
================================================================================================
The textures of the OpenGL backend.
================================================================================================
*/

#include "../tr_local.h"
#include "gl_Backend.h"

/*
================================================================================================

	A texture handle is a glTexture_t pointer.

	Creating, uploading, copying and changing the sampler bind the texture with glBindTexture on
	the unit GL has active, behind the back of the slots of the latch. Frames depend on what that
	leaves bound, so these binds stay exactly where they were.

================================================================================================
*/

static const GLuint TEXTURE_NOT_LOADED = 0xFFFFFFFF;

static bool GL_IsCompressed( const idImageOpts & opts ) {
	return ( opts.format == FMT_DXT1 || opts.format == FMT_DXT5 );
}

/*
========================
GL_SetTexParameters

The texture is bound on the active unit.
========================
*/
static void GL_SetTexParameters( const glTexture_t & tex, const samplerDesc_t & sampler ) {
	// ALPHA, LUMINANCE, LUMINANCE_ALPHA, and INTENSITY have been removed
	// in OpenGL 3.2. In order to mimic those modes, we use the swizzle operators
	if ( tex.opts.colorFormat == CFM_GREEN_ALPHA ) {
		qglTexParameteri( tex.target, GL_TEXTURE_SWIZZLE_R, GL_ONE );
		qglTexParameteri( tex.target, GL_TEXTURE_SWIZZLE_G, GL_ONE );
		qglTexParameteri( tex.target, GL_TEXTURE_SWIZZLE_B, GL_ONE );
		qglTexParameteri( tex.target, GL_TEXTURE_SWIZZLE_A, GL_GREEN );
	} else if ( tex.opts.format == FMT_LUM8 ) {
		qglTexParameteri( tex.target, GL_TEXTURE_SWIZZLE_R, GL_RED );
		qglTexParameteri( tex.target, GL_TEXTURE_SWIZZLE_G, GL_RED );
		qglTexParameteri( tex.target, GL_TEXTURE_SWIZZLE_B, GL_RED );
		qglTexParameteri( tex.target, GL_TEXTURE_SWIZZLE_A, GL_ONE );
	} else if ( tex.opts.format == FMT_L8A8 ) {
		qglTexParameteri( tex.target, GL_TEXTURE_SWIZZLE_R, GL_RED );
		qglTexParameteri( tex.target, GL_TEXTURE_SWIZZLE_G, GL_RED );
		qglTexParameteri( tex.target, GL_TEXTURE_SWIZZLE_B, GL_RED );
		qglTexParameteri( tex.target, GL_TEXTURE_SWIZZLE_A, GL_GREEN );
	} else if ( tex.opts.format == FMT_ALPHA ) {
		qglTexParameteri( tex.target, GL_TEXTURE_SWIZZLE_R, GL_ONE );
		qglTexParameteri( tex.target, GL_TEXTURE_SWIZZLE_G, GL_ONE );
		qglTexParameteri( tex.target, GL_TEXTURE_SWIZZLE_B, GL_ONE );
		qglTexParameteri( tex.target, GL_TEXTURE_SWIZZLE_A, GL_RED );
	} else if ( tex.opts.format == FMT_INT8 ) {
		qglTexParameteri( tex.target, GL_TEXTURE_SWIZZLE_R, GL_RED );
		qglTexParameteri( tex.target, GL_TEXTURE_SWIZZLE_G, GL_RED );
		qglTexParameteri( tex.target, GL_TEXTURE_SWIZZLE_B, GL_RED );
		qglTexParameteri( tex.target, GL_TEXTURE_SWIZZLE_A, GL_RED );
	} else {
		qglTexParameteri( tex.target, GL_TEXTURE_SWIZZLE_R, GL_RED );
		qglTexParameteri( tex.target, GL_TEXTURE_SWIZZLE_G, GL_GREEN );
		qglTexParameteri( tex.target, GL_TEXTURE_SWIZZLE_B, GL_BLUE );
		qglTexParameteri( tex.target, GL_TEXTURE_SWIZZLE_A, GL_ALPHA );
	}

	switch ( sampler.filter ) {
		case samplerFilter_t::LINEAR_MIP_LINEAR:
			qglTexParameterf( tex.target, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
			qglTexParameterf( tex.target, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
			break;
		case samplerFilter_t::LINEAR_MIP_NEAREST:
			qglTexParameterf( tex.target, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST );
			qglTexParameterf( tex.target, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
			break;
		case samplerFilter_t::LINEAR:
			qglTexParameterf( tex.target, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
			qglTexParameterf( tex.target, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
			break;
		case samplerFilter_t::NEAREST:
			qglTexParameterf( tex.target, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
			qglTexParameterf( tex.target, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
			break;
	}

	if ( glContext.anisotropicFilterAvailable ) {
		qglTexParameterf( tex.target, GL_TEXTURE_MAX_ANISOTROPY_EXT, sampler.maxAnisotropy );
	}
	if ( glContext.textureLODBiasAvailable ) {
		qglTexParameterf( tex.target, GL_TEXTURE_LOD_BIAS_EXT, sampler.lodBias );
	}

	// set the wrap/clamp modes
	switch ( sampler.address ) {
		case samplerAddress_t::REPEAT:
			qglTexParameterf( tex.target, GL_TEXTURE_WRAP_S, GL_REPEAT );
			qglTexParameterf( tex.target, GL_TEXTURE_WRAP_T, GL_REPEAT );
			break;
		case samplerAddress_t::BORDER_BLACK: {
			float color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
			qglTexParameterfv( tex.target, GL_TEXTURE_BORDER_COLOR, color );
			qglTexParameterf( tex.target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER );
			qglTexParameterf( tex.target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER );
			}
			break;
		case samplerAddress_t::BORDER_TRANSPARENT: {
			float color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			qglTexParameterfv( tex.target, GL_TEXTURE_BORDER_COLOR, color );
			qglTexParameterf( tex.target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER );
			qglTexParameterf( tex.target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER );
			}
			break;
		case samplerAddress_t::CLAMP:
			qglTexParameterf( tex.target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
			qglTexParameterf( tex.target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
			break;
	}
}

/*
========================
idRenderBackendGL::CreateTexture

Allocates all the necessary MipMap levels for the texture, but doesn't put anything in them.
OpenGL does not need the role: any texture can be the target of glCopyTexImage2D.
========================
*/
textureHandle_t idRenderBackendGL::CreateTexture( const idImageOpts & opts, [[maybe_unused]] textureRole_t role, const samplerDesc_t & sampler, const char * debugName ) {
	GL_CheckErrors();

	auto tex = std::make_unique<glTexture_t>();
	tex->texnum = TEXTURE_NOT_LOADED;
	tex->target = GL_TEXTURE_2D;
	tex->internalFormat = 0;
	tex->dataFormat = 0;
	tex->dataType = 0;
	tex->opts = opts;
#ifdef _DEBUG
	tex->name = debugName;
	tex->boundInFrame = -1;
#endif

	switch ( tex->opts.format ) {
	case FMT_RGBA8:
		tex->internalFormat = GL_RGBA8;
		tex->dataFormat = GL_RGBA;
		tex->dataType = GL_UNSIGNED_BYTE;
		break;
	case FMT_XRGB8:
		tex->internalFormat = GL_RGB;
		tex->dataFormat = GL_RGBA;
		tex->dataType = GL_UNSIGNED_BYTE;
		break;
	case FMT_RGB565:
		tex->internalFormat = GL_RGB;
		tex->dataFormat = GL_RGB;
		tex->dataType = GL_UNSIGNED_SHORT_5_6_5;
		break;
	case FMT_ALPHA:
		tex->internalFormat = GL_R8;
		tex->dataFormat = GL_RED;
		tex->dataType = GL_UNSIGNED_BYTE;
		break;
	case FMT_L8A8:
		tex->internalFormat = GL_RG8;
		tex->dataFormat = GL_RG;
		tex->dataType = GL_UNSIGNED_BYTE;
		break;
	case FMT_LUM8:
		tex->internalFormat = GL_R8;
		tex->dataFormat = GL_RED;
		tex->dataType = GL_UNSIGNED_BYTE;
		break;
	case FMT_INT8:
		tex->internalFormat = GL_R8;
		tex->dataFormat = GL_RED;
		tex->dataType = GL_UNSIGNED_BYTE;
		break;
	case FMT_DXT1:
		tex->internalFormat = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
		tex->dataFormat = GL_RGBA;
		tex->dataType = GL_UNSIGNED_BYTE;
		break;
	case FMT_DXT5:
		tex->internalFormat = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
		tex->dataFormat = GL_RGBA;
		tex->dataType = GL_UNSIGNED_BYTE;
		break;
	case FMT_DEPTH:
		tex->internalFormat = GL_DEPTH_COMPONENT;
		tex->dataFormat = GL_DEPTH_COMPONENT;
		tex->dataType = GL_UNSIGNED_BYTE;
		break;
	case FMT_X16:
		tex->internalFormat = GL_INTENSITY16;
		tex->dataFormat = GL_LUMINANCE;
		tex->dataType = GL_UNSIGNED_SHORT;
		break;
	case FMT_Y16_X16:
		tex->internalFormat = GL_LUMINANCE16_ALPHA16;
		tex->dataFormat = GL_LUMINANCE_ALPHA;
		tex->dataType = GL_UNSIGNED_SHORT;
		break;
	default:
		idLib::Error( "Unhandled image format %d in %s\n", tex->opts.format, debugName );
	}

	// generate the texture number
	qglGenTextures( 1, &tex->texnum );
	assert( tex->texnum != TEXTURE_NOT_LOADED );

	//----------------------------------------------------
	// allocate all the mip levels with NULL data
	//----------------------------------------------------

	int numSides;
	int target;
	int uploadTarget;
	if ( tex->opts.textureType == TT_2D ) {
		target = uploadTarget = GL_TEXTURE_2D;
		numSides = 1;
	} else if ( tex->opts.textureType == TT_CUBIC ) {
		target = GL_TEXTURE_CUBE_MAP_EXT;
		uploadTarget = GL_TEXTURE_CUBE_MAP_POSITIVE_X_EXT;
		numSides = 6;
	} else {
		assert( !"tex->opts.textureType" );
		target = uploadTarget = GL_TEXTURE_2D;
		numSides = 1;
	}

	qglBindTexture( target, tex->texnum );

	for ( int side = 0; side < numSides; side++ ) {
		int w = tex->opts.width;
		int h = tex->opts.height;
		if ( tex->opts.textureType == TT_CUBIC ) {
			h = w;
		}
		for ( int level = 0; level < tex->opts.numLevels; level++ ) {

			// clear out any previous error
			GL_CheckErrors();

			if ( GL_IsCompressed( tex->opts ) ) {
				int compressedSize = ( ((w+3)/4) * ((h+3)/4) * int64( 16 ) * BitsForFormat( tex->opts.format ) ) / 8;

				// Even though the OpenGL specification allows the 'data' pointer to be NULL, for some
				// drivers we actually need to upload data to get it to allocate the texture.
				// However, on 32-bit systems we may fail to allocate a large block of memory for large
				// textures. We handle this case by using HeapAlloc directly and allowing the allocation
				// to fail in which case we simply pass down NULL to glCompressedTexImage2D and hope for the best.
				// As of 2011-10-6 using NVIDIA hardware and drivers we have to allocate the memory with HeapAlloc
				// with the exact size otherwise large image allocation (for instance for physical page textures)
				// may fail on Vista 32-bit.
				void * data = HeapAlloc( GetProcessHeap(), 0, compressedSize );
				qglCompressedTexImage2DARB( uploadTarget+side, level, tex->internalFormat, w, h, 0, compressedSize, data );
				if ( data != NULL ) {
					HeapFree( GetProcessHeap(), 0, data );
				}
			} else {
				qglTexImage2D( uploadTarget + side, level, tex->internalFormat, w, h, 0, tex->dataFormat, tex->dataType, NULL );
			}

			GL_CheckErrors();

			w = Max( 1, w >> 1 );
			h = Max( 1, h >> 1 );
		}
	}

	qglTexParameteri( target, GL_TEXTURE_MAX_LEVEL, tex->opts.numLevels - 1 );

	// see if we messed anything up
	GL_CheckErrors();

	if ( tex->opts.textureType != TT_2D && tex->opts.textureType != TT_CUBIC ) {
		idLib::FatalError( "%s: bad texture type %d", debugName, tex->opts.textureType );
	}
	tex->target = target;
	GL_SetTexParameters( *tex, sampler );

	GL_CheckErrors();

	// without a context there is no name: the image stays unloaded
	if ( tex->texnum == TEXTURE_NOT_LOADED ) {
		return textureHandle_t::NONE;
	}
	return GL_TextureHandle( tex.release() );
}

/*
========================
idRenderBackendGL::API_DestroyTexture
========================
*/
void idRenderBackendGL::API_DestroyTexture( textureHandle_t texture ) {
	glTexture_t * tex = GL_Texture( texture );
	if ( tex == nullptr ) {
		return;
	}
	qglDeleteTextures( 1, &tex->texnum );	// this should be the ONLY place it is ever called!
	delete tex;
}

/*
========================
idRenderBackendGL::UploadTexture
========================
*/
void idRenderBackendGL::UploadTexture( textureHandle_t texture, int mipLevel, int face, int width, int height, const void * pic, int pixelPitch ) {
	const glTexture_t * tex = GL_Texture( texture );

#ifdef _DEBUG
	// rule R4: an upload into a texture this frame has already bound breaks the pass of a pipeline backend
	if ( tex->boundInFrame == debugFrame && debugUploadsAfterBind++ == 0 ) {
		common->Printf( "idRenderBackendGL::UploadTexture: an upload into '%s', which this frame has bound already\n", tex->name.c_str() );
	}
#endif

	int compressedSize = 0;

	if ( GL_IsCompressed( tex->opts ) ) {
		// compressed size may be larger than the dimensions due to padding to quads
		int quadW = ( width + 3 ) & ~3;
		int quadH = ( height + 3 ) & ~3;
		compressedSize = quadW * quadH * BitsForFormat( tex->opts.format ) / 8;

		// upload the non-aligned value, OpenGL understands that there
		// will be padding
		if ( width > tex->opts.width ) {
			width = tex->opts.width;
		}
		if ( height > tex->opts.height ) {
			height = tex->opts.height;
		}
	} else {
		assert( width <= tex->opts.width && height <= tex->opts.height );
	}

	int target;
	int uploadTarget;
	if ( tex->opts.textureType == TT_2D ) {
		target = GL_TEXTURE_2D;
		uploadTarget = GL_TEXTURE_2D;
	} else if ( tex->opts.textureType == TT_CUBIC ) {
		target = GL_TEXTURE_CUBE_MAP_EXT;
		uploadTarget = GL_TEXTURE_CUBE_MAP_POSITIVE_X_EXT + face;
	} else {
		assert( !"invalid tex->opts.textureType" );
		target = GL_TEXTURE_2D;
		uploadTarget = GL_TEXTURE_2D;
	}

	qglBindTexture( target, tex->texnum );

	if ( pixelPitch != 0 ) {
		qglPixelStorei( GL_UNPACK_ROW_LENGTH, pixelPitch );
	}
	if ( tex->opts.format == FMT_RGB565 ) {
		glPixelStorei( GL_UNPACK_SWAP_BYTES, GL_TRUE );
	}
#ifdef DEBUG
	GL_CheckErrors();
#endif
	if ( GL_IsCompressed( tex->opts ) ) {
		qglCompressedTexSubImage2DARB( uploadTarget, mipLevel, 0, 0, width, height, tex->internalFormat, compressedSize, pic );
	} else {

		// make sure the pixel store alignment is correct so that lower mips get created
		// properly for odd shaped textures - this fixes the mip mapping issues with
		// fonts
		int unpackAlignment = width * BitsForFormat( (textureFormat_t)tex->opts.format ) / 8;
		if ( ( unpackAlignment & 3 ) == 0 ) {
			qglPixelStorei( GL_UNPACK_ALIGNMENT, 4 );
		} else {
			qglPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
		}

		qglTexSubImage2D( uploadTarget, mipLevel, 0, 0, width, height, tex->dataFormat, tex->dataType, pic );
	}
#ifdef DEBUG
	GL_CheckErrors();
#endif
	if ( tex->opts.format == FMT_RGB565 ) {
		glPixelStorei( GL_UNPACK_SWAP_BYTES, GL_FALSE );
	}
	if ( pixelPitch != 0 ) {
		qglPixelStorei( GL_UNPACK_ROW_LENGTH, 0 );
	}
}

/*
========================
idRenderBackendGL::SetTextureSampler
========================
*/
void idRenderBackendGL::SetTextureSampler( textureHandle_t texture, const samplerDesc_t & sampler ) {
	const glTexture_t * tex = GL_Texture( texture );
	qglBindTexture( tex->target, tex->texnum );
	GL_SetTexParameters( *tex, sampler );
}

/*
========================
idRenderBackendGL::CopyOutputToTexture
========================
*/
void idRenderBackendGL::CopyOutputToTexture( textureHandle_t texture, int x, int y, int imageWidth, int imageHeight ) {
	glTexture_t * tex = GL_Texture( texture );

	qglBindTexture( tex->target, tex->texnum );

	qglReadBuffer( GL_BACK );

	tex->opts.width = imageWidth;
	tex->opts.height = imageHeight;
	qglCopyTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, x, y, imageWidth, imageHeight, 0 );

	// these shouldn't be necessary if the image was initialized properly
	qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );

	qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
}

/*
========================
idRenderBackendGL::CopyDepthToTexture
========================
*/
void idRenderBackendGL::CopyDepthToTexture( textureHandle_t texture, int x, int y, int imageWidth, int imageHeight ) {
	glTexture_t * tex = GL_Texture( texture );

	qglBindTexture( tex->target, tex->texnum );

	tex->opts.width = imageWidth;
	tex->opts.height = imageHeight;
	qglCopyTexImage2D( GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, x, y, imageWidth, imageHeight, 0 );
}

/*
========================
idRenderBackendGL::API_BindTexture
========================
*/
void idRenderBackendGL::API_BindTexture( int slot, textureKind_t kind, textureHandle_t texture ) {
	glTexture_t * tex = GL_Texture( texture );
	const GLenum target = ( kind == textureKind_t::TEX_CUBE ) ? GL_TEXTURE_CUBE_MAP_EXT : GL_TEXTURE_2D;
	qglBindMultiTextureEXT( GL_TEXTURE0_ARB + slot, target, ( tex != nullptr ) ? tex->texnum : 0 );
#ifdef _DEBUG
	if ( tex != nullptr ) {
		tex->boundInFrame = debugFrame;
	}
#endif
}
