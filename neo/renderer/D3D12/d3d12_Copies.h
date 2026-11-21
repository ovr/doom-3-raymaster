// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

#include "d3d12_InternalPasses.h"
#include "d3d12_Textures.h"

/*
	glCopyTexImage2D from the scene into a texture, for heat haze, glass, mirrors and remote views
	(RB_CopyRender), motion blur and resolution scaling. Rows keep GL order on both sides: scene row
	y + r becomes texture row r, so the box (x, y, width, height) of shared code is used as it is.

	  colour   the box through CopyTextureRegion into an R8G8B8A8_UNORM texture, or from the HDR scene into
	           an RGBA16F one; with MSAA the box is resolved first (ResolveSubresourceRegion), as GL resolves
	           a multisampled read
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
	// Leaves the scene colour (or its resolve texture) in COPY_SOURCE and the target in COPY_DEST. hdr: the HDR
	// scene into a float surface (R13).
	void							Color( textureHandle_t texture, int x, int y, int width, int height, bool hdr = false );
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
