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

#pragma hdrstop
#include "../../idlib/precompiled.h"

#include "../tr_local.h"
#include "d3d12_Draw.h"

idD3D12DrawRecorder::idD3D12DrawRecorder( idD3D12Submitter & submit, idD3D12Barriers & batch, idD3D12SceneTarget & target,
	idD3D12InternalPasses & internalPasses ) :
	submitter( submit ),
	barriers( batch ),
	scene( target ),
	passes( internalPasses ) {
}

void idD3D12DrawRecorder::BeginPass( ID3D12GraphicsCommandList1 & list ) {
	idD3D12SceneTarget::surface_t & color = scene.Color();
	idD3D12SceneTarget::surface_t & depth = scene.Depth();
	barriers.Transition( *color.resource.Get(), color.state, D3D12_RESOURCE_STATE_RENDER_TARGET );
	barriers.Transition( *depth.resource.Get(), depth.state, D3D12_RESOURCE_STATE_DEPTH_WRITE );
	barriers.Flush();
	const D3D12_CPU_DESCRIPTOR_HANDLE rtv = scene.Rtv( false );
	const D3D12_CPU_DESCRIPTOR_HANDLE dsv = scene.Dsv();
	list.OMSetRenderTargets( 1, &rtv, FALSE, &dsv );
	bound.passOpen = true;
}

// The scene is in GL row order, so the rectangle of shared code is used as it is. glClear obeys
// glDepthMask; the stencil write mask is never changed from 0xFF.
void idD3D12DrawRecorder::Clear( const renderLatch_t & latch, const clearDesc_t & clear ) {
	const int x0 = Max( clear.rect.x, 0 );
	const int y0 = Max( clear.rect.y, 0 );
	const int x1 = Min( clear.rect.x + clear.rect.width, scene.Width() );
	const int y1 = Min( clear.rect.y + clear.rect.height, scene.Height() );
	if ( x1 <= x0 || y1 <= y0 ) {
		return;
	}
	const uint8 mask = clear.color ? static_cast<uint8>( clear.colorWriteMask & 0xF ) : 0;
	D3D12_CLEAR_FLAGS depthStencil = {};
	if ( clear.depth && ( latch.stateBits & GLS_DEPTHMASK ) == 0 ) {
		depthStencil |= D3D12_CLEAR_FLAG_DEPTH;
	}
	if ( clear.stencil ) {
		depthStencil |= D3D12_CLEAR_FLAG_STENCIL;
	}

	if ( mask == 0xF || depthStencil != 0 ) {
		ID3D12GraphicsCommandList1 & list = submitter.List();
		if ( !bound.passOpen ) {
			BeginPass( list );
		}
		const D3D12_RECT rect = { x0, y0, x1, y1 };
		if ( mask == 0xF ) {
			list.ClearRenderTargetView( scene.Rtv( false ), clear.rgba, 1, &rect );
		}
		if ( depthStencil != 0 ) {
			list.ClearDepthStencilView( scene.Dsv(), depthStencil, 1.0f, clear.stencilValue, 1, &rect );
		}
	}
	if ( mask != 0 && mask != 0xF ) {
		idD3D12SceneTarget::surface_t & color = scene.Color();
		barriers.Transition( *color.resource.Get(), color.state, D3D12_RESOURCE_STATE_RENDER_TARGET );
		passes.MaskedClear( scene.Rtv( false ), scene.Target( false ), { x0, y0, x1 - x0, y1 - y0 }, clear.rgba, mask );
		Invalidate();
	}
}
