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

// What the open list has bound. Empty means unknown: a new list and every internal pass clear it.
struct d3d12BoundState_t {
	bool							passOpen = false;	// scene RTV + DSV set, colour in RENDER_TARGET, depth in DEPTH_WRITE
};

// The latch and the calls of the contract → commands; sends only what differs from d3d12BoundState_t.
class idD3D12DrawRecorder {
public:
									idD3D12DrawRecorder( idD3D12Submitter & submitter, idD3D12Barriers & barriers, idD3D12SceneTarget & scene,
										idD3D12InternalPasses & passes );
	// As glClear: inside the latched scissor, through the latched write masks; a partial colour mask is a pass.
	void							Clear( const renderLatch_t & latch, const clearDesc_t & clear );
	void							EndPass() { bound.passOpen = false; }	// R10: before a copy, a readback, the blit and in EndFrame
	void							Invalidate() { bound = {}; }			// a new list or an internal pass

private:
	void							BeginPass( ID3D12GraphicsCommandList1 & list );	// R10: the first draw or clear after EndPass

	idD3D12Submitter &				submitter;
	idD3D12Barriers &				barriers;
	idD3D12SceneTarget &			scene;
	idD3D12InternalPasses &			passes;
	d3d12BoundState_t				bound;
};
