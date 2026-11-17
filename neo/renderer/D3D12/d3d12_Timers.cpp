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
#include "d3d12_Timers.h"

namespace {

constexpr UINT PIX_EVENT_UNICODE = 0;	// PIX's metadata for a wide string with its terminator

}	// namespace

void idD3D12DebugGroups::Begin( ID3D12GraphicsCommandList1 & list, int level ) const {
	const std::array<wchar_t, MAX_NAME> & name = names[static_cast<size_t>( level )];
	const size_t length = std::char_traits<wchar_t>::length( name.data() );
	list.BeginEvent( PIX_EVENT_UNICODE, name.data(), static_cast<UINT>( ( length + 1 ) * sizeof( wchar_t ) ) );
}

// Engine names are ASCII: each byte widens to one UTF-16 unit.
void idD3D12DebugGroups::Push( ID3D12GraphicsCommandList1 & list, const char * name ) {
	if ( depth == MAX_DEPTH ) {
		ignored++;
		return;
	}
	std::array<wchar_t, MAX_NAME> & wide = names[static_cast<size_t>( depth )];
	size_t length = 0;
	for ( ; name[length] != '\0' && length + 1 < wide.size(); length++ ) {
		wide[length] = static_cast<wchar_t>( static_cast<unsigned char>( name[length] ) );
	}
	wide[length] = L'\0';
	Begin( list, depth );
	depth++;
	pushed++;
}

void idD3D12DebugGroups::Pop( ID3D12GraphicsCommandList1 * list ) {
	if ( ignored > 0 ) {
		ignored--;
		return;
	}
	if ( depth == 0 ) {
		return;
	}
	depth--;
	if ( list != nullptr ) {
		list->EndEvent();
	}
}

void idD3D12DebugGroups::CloseAll( ID3D12GraphicsCommandList1 & list ) {
	for ( int level = 0; level < depth; level++ ) {
		list.EndEvent();
	}
}

void idD3D12DebugGroups::Reopen( ID3D12GraphicsCommandList1 & list ) {
	for ( int level = 0; level < depth; level++ ) {
		Begin( list, level );
	}
}
