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
	The debug groups of r_pix 2 for PIX and RenderDoc: BeginEvent / EndEvent with a UTF-16 name
	(metadata 0, PIX's unicode event). A group of shared code can outlive a list, so the stack lives
	here, not in the list: SubmitChunk ends every open group in the list it closes, and the next list
	begins them again in the same order. While a list is open, every group of the stack is open in it.

	  Push   → BeginEvent in the open list (opened on demand)
	  Pop    → EndEvent if a list is open
	  CloseAll (SubmitChunk), Reopen (a new list)
*/
class idD3D12DebugGroups {
public:
	static constexpr int			MAX_DEPTH = 64;		// caps.maxDebugGroupDepth
	static constexpr int			MAX_NAME = 64;		// characters with the terminator; longer names are cut

	void							Push( ID3D12GraphicsCommandList1 & list, const char * name );
	void							Pop( ID3D12GraphicsCommandList1 * list );	// nullptr: no list is open
	void							CloseAll( ID3D12GraphicsCommandList1 & list );
	void							Reopen( ID3D12GraphicsCommandList1 & list );
	[[nodiscard]] int				Depth() const { return depth; }
	[[nodiscard]] int				PushedThisFrame() const { return pushed; }
	void							ResetFrameCounters() { pushed = 0; }

private:
	void							Begin( ID3D12GraphicsCommandList1 & list, int level ) const;

	std::array<std::array<wchar_t, MAX_NAME>, MAX_DEPTH>	names = {};
	int								depth = 0;
	int								ignored = 0;		// pushes past MAX_DEPTH, so the pops stay balanced
	int								pushed = 0;
};
