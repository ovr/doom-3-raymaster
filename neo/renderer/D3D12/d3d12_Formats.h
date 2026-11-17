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

#pragma once

#include "d3d12_Local.h"

/*
	The texture formats of shared code (ImageOpts.h) as DXGI formats and SRV component mappings, so a
	shader samples what GL's swizzles (gl_Image.cpp GL_SetTexParameters) give:

	  format                  resource / view          mapping   bytes      upload
	  RGBA8, COLOR_COPY role  R8G8B8A8_UNORM           R G B A   4          copied
	  DEPTH_COPY role, DEPTH  R32_FLOAT, + RTV         R R R 1   4          copied (the role wins over the format)
	  XRGB8                   R8G8B8A8_UNORM           R G B 1   4          copied
	  ALPHA                   R8_UNORM                 1 1 1 R   1          copied
	  L8A8                    R8G8_UNORM               R R R G   2          copied
	  LUM8                    R8_UNORM                 R R R 1   1          copied
	  INT8                    R8_UNORM                 R R R R   1          copied
	  DXT1                    BC1_UNORM                R G B A   8 / block  copied
	  DXT5                    BC3_UNORM                R G B A   16 / block copied
	  DXT1/5, unaligned base  R8G8B8A8_UNORM           as above  4          idDxtDecoder (no OPTIONS8)
	  RGB565                  B5G6R5_UNORM             R G B 1   2          bytes swapped (big-endian words)
	  X16                     R16_UNORM                R R R R   2          copied
	  Y16_X16                 R16G16_UNORM             R R R G   4          copied
	  NONE                    -                        -         -          Create warns, returns NONE
	CFM_GREEN_ALPHA maps 1 1 1 G for any format: GL tests it before the format.

	Q3, the border colour under a mapping: GL swizzles the border, so INT8 with BORDER_BLACK samples
	(0,0,0,0) outside and ALPHA (1,1,1,0). D3D12 applies the mapping to the border too (the Borders
	tests, on WARP and an RX 7900 XTX), so INT8 stays R8 with R R R R and needs no expansion to RGBA8.
*/

enum class d3d12Convert_t : uint8 { NONE, SWAP_565, DECODE_DXT1, DECODE_DXT5 };

struct d3d12FormatInfo_t {
	DXGI_FORMAT						resource = DXGI_FORMAT_UNKNOWN;	// UNKNOWN: no texture of this format
	DXGI_FORMAT						srv = DXGI_FORMAT_UNKNOWN;
	DXGI_FORMAT						rtv = DXGI_FORMAT_UNKNOWN;		// DEPTH_COPY only: CopyDepthToTexture draws into it
	UINT							mapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	uint8							blockBytes = 0;		// of the resource: per pixel, or per 4 x 4 block when compressed
	uint8							sourceBytes = 0;	// of the data shared code uploads, per pixel or per block
	bool							compressed = false;	// the resource is block compressed
	bool							sourceBlocks = false;	// the uploaded data is DXT blocks: compressed or decoded
	d3d12Convert_t					convert = d3d12Convert_t::NONE;
	constexpr bool					operator==( const d3d12FormatInfo_t & ) const = default;
};

// width, height: of level 0 (a cube's height is its width). unalignedBlocks: OPTIONS8, a BC base level of
// any size; without it a BC texture whose base is not a multiple of 4 is decoded into RGBA8.
[[nodiscard]] d3d12FormatInfo_t	D3D12_FormatFor( textureFormat_t format, textureColor_t color, textureRole_t role, int width, int height,
									bool unalignedBlocks );
