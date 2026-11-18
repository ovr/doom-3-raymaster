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

#include "d3d12_InternalPasses.h"
#include "d3d12_Textures.h"

/*
	glCopyTexImage2D from the scene into a texture, for heat haze, glass, mirrors and remote views
	(RB_CopyRender), motion blur and resolution scaling. Rows keep GL order on both sides: scene row
	y + r becomes texture row r, so the box (x, y, width, height) of shared code is used as it is.

	  colour   the box through CopyTextureRegion into an R8G8B8A8_UNORM texture; with MSAA the box is
	           resolved first (ResolveSubresourceRegion), as GL resolves a multisampled read
	  depth    the depth-copy pass into an R32_FLOAT texture: a depth-stencil resource is copied whole
	           or not at all, and GL's copy keeps the 24-bit depth as a value, sample 0 under MSAA

	The pass is ended first (R10). The target gets the size of the box (idD3D12TextureStore::PrepareCopyTarget),
	and texels whose pixel lies outside the scene are undefined, as in GL.
*/
class idD3D12ScreenCopies {
public:
	struct stats_t {
		int							color = 0;
		int							depth = 0;
	};

									idD3D12ScreenCopies( idD3D12Submitter & submitter, idD3D12Barriers & barriers, idD3D12SceneTarget & scene,
										idD3D12TextureStore & textures, idD3D12InternalPasses & passes, idD3D12DescriptorPool & rtvPool );
	// Leaves the scene colour (or its resolve texture) in COPY_SOURCE and the target in COPY_DEST.
	void							Color( textureHandle_t texture, int x, int y, int width, int height );
	// An internal pass: the caller invalidates the recorder. Leaves the scene depth in DEPTH_READ | PIXEL_SHADER_RESOURCE.
	void							Depth( textureHandle_t texture, int x, int y, int width, int height );
	[[nodiscard]] stats_t			TakeStats();	// the frame's, and zero again

private:
	idD3D12Submitter &				submitter;
	idD3D12Barriers &				barriers;
	idD3D12SceneTarget &			scene;
	idD3D12TextureStore &			textures;
	idD3D12InternalPasses &			passes;
	idD3D12DescriptorPool &			rtvPool;
	stats_t							stats;
};
