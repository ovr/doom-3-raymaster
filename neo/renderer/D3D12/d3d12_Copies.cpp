// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "../../idlib/precompiled.h"

#include "../tr_local.h"
#include "d3d12_Copies.h"

idD3D12ScreenCopies::idD3D12ScreenCopies( idD3D12Submitter & submit, idD3D12Barriers & batch, idD3D12SceneTarget & target,
	idD3D12TextureStore & store, idD3D12InternalPasses & internalPasses, idD3D12DescriptorPool & rtvs ) :
	submitter( submit ),
	barriers( batch ),
	scene( target ),
	textures( store ),
	passes( internalPasses ),
	rtvPool( rtvs ) {
}

// Only the part of the box inside the scene is copied; it lands at its offset in the texture.
void idD3D12ScreenCopies::Color( textureHandle_t texture, int x, int y, int width, int height ) {
	d3d12TextureSurface_t * target = textures.PrepareCopyTarget( texture, d3d12CopyKind_t::COLOR, width, height );
	if ( target == nullptr ) {
		return;
	}
	stats.color++;
	const int x0 = Max( x, 0 );
	const int y0 = Max( y, 0 );
	const int x1 = Min( x + width, scene.Width() );
	const int y1 = Min( y + height, scene.Height() );
	if ( x1 <= x0 || y1 <= y0 ) {
		return;
	}
	const D3D12_RECT region = { x0, y0, x1, y1 };
	idD3D12SceneTarget::surface_t & source = scene.Resolved( barriers, submitter, region );
	barriers.Transition( *source.resource.Get(), source.state, D3D12_RESOURCE_STATE_COPY_SOURCE );
	barriers.Transition( *target->resource.Get(), target->state, D3D12_RESOURCE_STATE_COPY_DEST );
	barriers.Flush();

	D3D12_TEXTURE_COPY_LOCATION to = {};
	to.pResource = target->resource.Get();
	to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	to.SubresourceIndex = 0;
	D3D12_TEXTURE_COPY_LOCATION from = {};
	from.pResource = source.resource.Get();
	from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	from.SubresourceIndex = 0;
	const D3D12_BOX box = { static_cast<UINT>( x0 ), static_cast<UINT>( y0 ), 0, static_cast<UINT>( x1 ), static_cast<UINT>( y1 ), 1 };
	submitter.List().CopyTextureRegion( &to, static_cast<UINT>( x0 - x ), static_cast<UINT>( y0 - y ), 0, &from, &box );
}

void idD3D12ScreenCopies::Depth( textureHandle_t texture, int x, int y, int width, int height ) {
	d3d12TextureSurface_t * target = textures.PrepareCopyTarget( texture, d3d12CopyKind_t::DEPTH, width, height );
	if ( target == nullptr ) {
		return;
	}
	stats.depth++;
	idD3D12SceneTarget::surface_t & depth = scene.Depth();
	barriers.Transition( *depth.resource.Get(), depth.state, D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
	barriers.Transition( *target->resource.Get(), target->state, D3D12_RESOURCE_STATE_RENDER_TARGET );
	passes.CopyDepth( depth.srv, scene.Samples(), rtvPool.Cpu( target->rtv ), x, y, width, height );
}

idD3D12ScreenCopies::stats_t idD3D12ScreenCopies::TakeStats() {
	const stats_t taken = stats;
	stats = {};
	return taken;
}
