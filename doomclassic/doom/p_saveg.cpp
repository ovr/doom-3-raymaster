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

#include "Precompiled.h"
#include "globaldata.h"

#include "i_system.h"
#include "z_zone.h"
#include "p_local.h"

// State.
#include "doomstat.h"
#include "r_state.h"

#include <iterator>
#include <type_traits>
#include <unordered_map>
#include <vector>

// A save holds whole struct copies, so it loads only into the struct layout that wrote it (the
// version string in g_game.cpp rejects others). Pointer fields carry indices or are cleared, and
// every access goes through memcpy, so unaligned data is safe.

namespace {

// Keeps struct copies pointer-aligned, counted from the buffer start so that save and load agree.
void PadSave() {
	constexpr ptrdiff_t align = alignof( void * );
	const ptrdiff_t offset = ::g->save_p - ::g->savebuffer;
	::g->save_p += ( align - ( offset & ( align - 1 ) ) ) & ( align - 1 );
}

template< typename T >
void SaveValue( const T & value ) {
	static_assert( std::is_trivially_copyable_v< T > );
	memcpy( ::g->save_p, &value, sizeof( T ) );
	::g->save_p += sizeof( T );
}

template< typename T >
T LoadValue() {
	static_assert( std::is_trivially_copyable_v< T > );
	T value;
	memcpy( &value, ::g->save_p, sizeof( T ) );
	::g->save_p += sizeof( T );
	return value;
}

void SaveByte( int value ) {
	*::g->save_p++ = static_cast< byte >( value );
}

byte LoadByte() {
	return *::g->save_p++;
}

void SaveShort( int value ) {
	SaveValue( static_cast< short >( value ) );
}

short LoadShort() {
	return LoadValue< short >();
}

// An array index kept in a pointer field of a saved struct copy.
template< typename T >
T * IndexAsPointer( ptrdiff_t index ) {
	return reinterpret_cast< T * >( static_cast< intptr_t >( index ) );
}

template< typename T >
ptrdiff_t PointerAsIndex( T * field ) {
	return static_cast< ptrdiff_t >( reinterpret_cast< intptr_t >( field ) );
}

bool IsMobj( const thinker_t * th ) {
	return th->function.acp1 == (actionf_p1)P_MobjThinker;
}

template< typename T, size_t N >
bool IsListed( const thinker_t * th, T * ( &list )[N] ) {
	for ( const T * entry : list ) {
		if ( entry != nullptr && &entry->thinker == th ) {
			return true;
		}
	}
	return false;
}

// The blockmap cell the mobj is linked into, NULL outside the blockmap.
mobj_t ** BlockLinkHead( const mobj_t * mobj ) {
	const int blockx = ( mobj->x - ::g->bmaporgx ) >> MAPBLOCKSHIFT;
	const int blocky = ( mobj->y - ::g->bmaporgy ) >> MAPBLOCKSHIFT;
	if ( blockx < 0 || blockx >= ::g->bmapwidth || blocky < 0 || blocky >= ::g->bmapheight ) {
		return nullptr;
	}
	return &::g->blocklinks[ blocky * ::g->bmapwidth + blockx ];
}

// Links between mobjs, saved after each mobj as 1-based indices in save order (0 is NULL).
struct mobjLinks_t {
	int32_t	target;
	int32_t	tracer;
	int32_t	snext;
	int32_t	sprev;
	int32_t	bnext;
	int32_t	bprev;
	bool	sectorHead;		// first in its sector's thing list
	bool	blockHead;		// first in its blockmap cell
};

// A special points at its sector; the thinker function is saved as a flag, NULL in stasis.
template< typename T >
void ArchiveSpecial( specials_e tclass, const thinker_t * th ) {
	T special = *reinterpret_cast< const T * >( th );
	special.thinker = {};
	special.sector = IndexAsPointer< sector_t >( special.sector - ::g->sectors );

	SaveByte( tclass );
	SaveByte( th->function.acv != nullptr );
	PadSave();
	SaveValue( special );
}

template< typename T >
T * UnArchiveSpecial( actionf_p1 think ) {
	const bool active = LoadByte() != 0;
	PadSave();

	T * special = static_cast< T * >( DoomLib::Z_Malloc( static_cast< int >( sizeof( T ) ), PU_LEVEL, nullptr ) );
	*special = LoadValue< T >();
	special->sector = &::g->sectors[ PointerAsIndex( special->sector ) ];
	special->thinker.function.acp1 = active ? think : nullptr;
	P_AddThinker( &special->thinker );
	return special;
}

} // namespace



//
// P_ArchivePlayers
//
void P_ArchivePlayers (void)
{
	for ( int i = 0; i < MAXPLAYERS; i++ ) {
		if ( !::g->playeringame[i] ) {
			continue;
		}

		// mo is relinked by the player's mobj on load
		player_t player = ::g->players[i];
		player.mo = nullptr;
		player.message = nullptr;
		player.attacker = nullptr;
		for ( pspdef_t & psp : player.psprites ) {
			if ( psp.state ) {
				psp.state = IndexAsPointer< const state_t >( psp.state - ::g->states );
			}
		}

		PadSave();
		SaveValue( player );
	}
}



//
// P_UnArchivePlayers
//
void P_UnArchivePlayers (void)
{
	for ( int i = 0; i < MAXPLAYERS; i++ ) {
		if ( !::g->playeringame[i] ) {
			continue;
		}

		PadSave();
		player_t & player = ::g->players[i];
		player = LoadValue< player_t >();

		// will be set when unarc thinker
		player.mo = nullptr;
		player.message = nullptr;
		player.attacker = nullptr;
		for ( pspdef_t & psp : player.psprites ) {
			if ( psp.state ) {
				psp.state = &::g->states[ PointerAsIndex( psp.state ) ];
			}
		}
	}
}


//
// P_ArchiveWorld
//
void P_ArchiveWorld (void)
{
	const sector_t * sec = ::g->sectors;
	for ( int i = 0; i < ::g->numsectors; i++, sec++ ) {
		SaveShort( sec->floorheight >> FRACBITS );
		SaveShort( sec->ceilingheight >> FRACBITS );
		SaveShort( sec->floorpic );
		SaveShort( sec->ceilingpic );
		SaveShort( sec->lightlevel );
		SaveShort( sec->special );		// needed?
		SaveShort( sec->tag );		// needed?
	}

	const line_t * li = ::g->lines;
	for ( int i = 0; i < ::g->numlines; i++, li++ ) {
		SaveShort( li->flags );
		SaveShort( li->special );
		SaveShort( li->tag );
		for ( const short sidenum : li->sidenum ) {
			if ( sidenum == -1 ) {
				continue;
			}

			const side_t & si = ::g->sides[ sidenum ];
			SaveShort( si.textureoffset >> FRACBITS );
			SaveShort( si.rowoffset >> FRACBITS );
			SaveShort( si.toptexture );
			SaveShort( si.bottomtexture );
			SaveShort( si.midtexture );
		}
	}

	// Doom 2 level 30 requires some global pointers, wheee!
	SaveShort( ::g->braintargeton );
	SaveShort( ::g->easy );
}



//
// P_UnArchiveWorld
//
void P_UnArchiveWorld (void)
{
	sector_t * sec = ::g->sectors;
	for ( int i = 0; i < ::g->numsectors; i++, sec++ ) {
		sec->floorheight = LoadShort() << FRACBITS;
		sec->ceilingheight = LoadShort() << FRACBITS;
		sec->floorpic = LoadShort();
		sec->ceilingpic = LoadShort();
		sec->lightlevel = LoadShort();
		sec->special = LoadShort();		// needed?
		sec->tag = LoadShort();		// needed?
		sec->specialdata = nullptr;
		sec->soundtarget = nullptr;
	}

	line_t * li = ::g->lines;
	for ( int i = 0; i < ::g->numlines; i++, li++ ) {
		li->flags = LoadShort();
		li->special = LoadShort();
		li->tag = LoadShort();
		for ( const short sidenum : li->sidenum ) {
			if ( sidenum == -1 ) {
				continue;
			}

			side_t & si = ::g->sides[ sidenum ];
			si.textureoffset = LoadShort() << FRACBITS;
			si.rowoffset = LoadShort() << FRACBITS;
			si.toptexture = LoadShort();
			si.bottomtexture = LoadShort();
			si.midtexture = LoadShort();
		}
	}

	// Doom 2 level 30 requires some global pointers, wheee!
	::g->braintargeton = LoadShort();
	::g->easy = LoadShort();
}





//
// Thinkers
//

//
// P_ArchiveThinkers
// The specials (ceilings, doors, floors, plats, lights) are archived with the mobjs.
//
void P_ArchiveThinkers (void)
{
	// Mobjs are numbered in thinker order, the order they are saved and loaded in.
	std::unordered_map< const mobj_t *, int32_t > mobjIndex;
	for ( thinker_t * th = ::g->thinkercap.next; th != &::g->thinkercap; th = th->next ) {
		if ( IsMobj( th ) ) {
			const int32_t index = static_cast< int32_t >( mobjIndex.size() ) + 1;
			mobjIndex.emplace( reinterpret_cast< const mobj_t * >( th ), index );
		}
	}
	const auto indexOf = [&mobjIndex]( const mobj_t * mobj ) -> int32_t {
		const auto it = mobjIndex.find( mobj );
		return it != mobjIndex.end() ? it->second : 0;
	};

	for ( thinker_t * th = ::g->thinkercap.next; th != &::g->thinkercap; th = th->next ) {
		if ( IsMobj( th ) ) {
			const mobj_t * mobj = reinterpret_cast< const mobj_t * >( th );

			mobjLinks_t links = {};
			links.target = indexOf( mobj->target );
			links.tracer = indexOf( mobj->tracer );
			links.snext = indexOf( mobj->snext );
			links.sprev = indexOf( mobj->sprev );
			links.bnext = indexOf( mobj->bnext );
			links.bprev = indexOf( mobj->bprev );
			links.sectorHead = mobj->subsector->sector->thinglist == mobj;
			mobj_t * const * blockHead = BlockLinkHead( mobj );
			links.blockHead = blockHead != nullptr && *blockHead == mobj;

			// The links, subsector and info are rebuilt on load.
			mobj_t copy = *mobj;
			copy.thinker = {};
			copy.snext = copy.sprev = nullptr;
			copy.bnext = copy.bprev = nullptr;
			copy.target = copy.tracer = nullptr;
			copy.subsector = nullptr;
			copy.info = nullptr;
			copy.state = IndexAsPointer< const state_t >( mobj->state - ::g->states );
			copy.player = mobj->player != nullptr ? IndexAsPointer< player_t >( mobj->player - ::g->players + 1 ) : nullptr;

			SaveByte( tc_mobj );
			PadSave();
			SaveValue( copy );
			SaveValue( links );
			continue;
		}

		// in stasis
		if ( th->function.acv == nullptr ) {
			if ( IsListed( th, ::g->activeceilings ) ) {
				ArchiveSpecial< ceiling_t >( tc_ceiling, th );
			} else if ( IsListed( th, ::g->activeplats ) ) {
				ArchiveSpecial< plat_t >( tc_plat, th );
			}
			continue;
		}

		const actionf_p1 think = th->function.acp1;
		if ( think == (actionf_p1)T_MoveCeiling ) {
			ArchiveSpecial< ceiling_t >( tc_ceiling, th );
		} else if ( think == (actionf_p1)T_VerticalDoor ) {
			ArchiveSpecial< vldoor_t >( tc_door, th );
		} else if ( think == (actionf_p1)T_MoveFloor ) {
			ArchiveSpecial< floormove_t >( tc_floor, th );
		} else if ( think == (actionf_p1)T_PlatRaise ) {
			ArchiveSpecial< plat_t >( tc_plat, th );
		} else if ( think == (actionf_p1)T_FireFlicker ) {
			ArchiveSpecial< fireflicker_t >( tc_fire, th );
		} else if ( think == (actionf_p1)T_LightFlash ) {
			ArchiveSpecial< lightflash_t >( tc_flash, th );
		} else if ( think == (actionf_p1)T_StrobeFlash ) {
			ArchiveSpecial< strobe_t >( tc_strobe, th );
		} else if ( think == (actionf_p1)T_Glow ) {
			ArchiveSpecial< glow_t >( tc_glow, th );
		}
	}

	// add a terminating marker
	SaveByte( tc_end );

	const sector_t * sec = ::g->sectors;
	for ( int i = 0; i < ::g->numsectors; i++, sec++ ) {
		SaveValue( indexOf( sec->soundtarget ) );
	}

	// add a terminating marker
	SaveByte( tc_end );
}



//
// P_UnArchiveThinkers
//
void P_UnArchiveThinkers (void)
{
	// remove all the current thinkers
	thinker_t * currentthinker = ::g->thinkercap.next;
	while ( currentthinker != &::g->thinkercap ) {
		thinker_t * next = currentthinker->next;

		if ( IsMobj( currentthinker ) ) {
			P_RemoveMobj( (mobj_t *)currentthinker );
		} else {
			Z_Free( currentthinker );
		}

		currentthinker = next;
	}

	P_InitThinkers ();

	std::vector< mobj_t * > mobjs;
	std::vector< mobjLinks_t > mobjLinks;
	const auto mobjAt = [&mobjs]( int32_t index ) -> mobj_t * {
		return index > 0 && index <= static_cast< int32_t >( mobjs.size() ) ? mobjs[ index - 1 ] : nullptr;
	};

	// read in saved thinkers
	for ( byte tclass = LoadByte(); tclass != tc_end; tclass = LoadByte() ) {
		switch ( tclass ) {
			case tc_mobj: {
				PadSave();
				mobj_t * mobj = static_cast< mobj_t * >( DoomLib::Z_Malloc( static_cast< int >( sizeof( mobj_t ) ), PU_LEVEL, nullptr ) );
				*mobj = LoadValue< mobj_t >();
				mobjLinks.push_back( LoadValue< mobjLinks_t >() );

				mobj->state = &::g->states[ PointerAsIndex( mobj->state ) ];
				if ( mobj->player != nullptr ) {
					mobj->player = &::g->players[ PointerAsIndex( mobj->player ) - 1 ];
					mobj->player->mo = mobj;
				}

				P_SetThingPosition (mobj);

				mobj->info = &mobjinfo[mobj->type];
				mobj->floorz = mobj->subsector->sector->floorheight;
				mobj->ceilingz = mobj->subsector->sector->ceilingheight;
				mobj->thinker.function.acp1 = (actionf_p1)P_MobjThinker;

				P_AddThinker (&mobj->thinker);
				mobjs.push_back( mobj );
				break;
			}

			case tc_ceiling: {
				ceiling_t * ceiling = UnArchiveSpecial< ceiling_t >( (actionf_p1)T_MoveCeiling );
				ceiling->sector->specialdata = ceiling;
				P_AddActiveCeiling( ceiling );
				break;
			}

			case tc_door: {
				vldoor_t * door = UnArchiveSpecial< vldoor_t >( (actionf_p1)T_VerticalDoor );
				door->sector->specialdata = door;
				break;
			}

			case tc_floor: {
				floormove_t * floor = UnArchiveSpecial< floormove_t >( (actionf_p1)T_MoveFloor );
				floor->sector->specialdata = floor;
				break;
			}

			case tc_plat: {
				plat_t * plat = UnArchiveSpecial< plat_t >( (actionf_p1)T_PlatRaise );
				plat->sector->specialdata = plat;
				P_AddActivePlat( plat );
				break;
			}

			case tc_fire:
				UnArchiveSpecial< fireflicker_t >( (actionf_p1)T_FireFlicker );
				break;

			case tc_flash:
				UnArchiveSpecial< lightflash_t >( (actionf_p1)T_LightFlash );
				break;

			case tc_strobe:
				UnArchiveSpecial< strobe_t >( (actionf_p1)T_StrobeFlash );
				break;

			case tc_glow:
				UnArchiveSpecial< glow_t >( (actionf_p1)T_Glow );
				break;

			default:
				I_Error ("Unknown tclass %i in savegame",tclass);
		}
	}

	// clear sector thing lists and blockmap thing lists, the saved links rebuild them
	sector_t * sec = ::g->sectors;
	for ( int i = 0; i < ::g->numsectors; i++, sec++ ) {
		sec->thinglist = nullptr;
	}
	memset( ::g->blocklinks, 0, sizeof( *::g->blocklinks ) * ::g->bmapwidth * ::g->bmapheight );

	// Doom 2 level 30 requires some global pointers, wheee!
	::g->numbraintargets = 0;
	const int maxBrainTargets = static_cast< int >( std::size( ::g->braintargets ) );

	// fixup mobj_t pointers now that all thinkers have been restored
	for ( size_t i = 0; i < mobjs.size(); i++ ) {
		mobj_t * mobj = mobjs[i];
		const mobjLinks_t & links = mobjLinks[i];

		mobj->target = mobjAt( links.target );
		mobj->tracer = mobjAt( links.tracer );
		mobj->snext = mobjAt( links.snext );
		mobj->sprev = mobjAt( links.sprev );
		mobj->bnext = mobjAt( links.bnext );
		mobj->bprev = mobjAt( links.bprev );

		if ( links.sectorHead ) {
			mobj->subsector->sector->thinglist = mobj;
		}

		if ( links.blockHead ) {
			if ( mobj_t ** head = BlockLinkHead( mobj ) ) {
				*head = mobj;
			}
		}

		if ( mobj->type == MT_BOSSTARGET && ::g->numbraintargets < maxBrainTargets ) {
			::g->braintargets[::g->numbraintargets++] = mobj;
		}
	}

	sec = ::g->sectors;
	for ( int i = 0; i < ::g->numsectors; i++, sec++ ) {
		sec->soundtarget = mobjAt( LoadValue< int32_t >() );
	}

	if ( LoadByte() != tc_end ) {
		I_Error( "Savegame error after loading sector soundtargets." );
	}
}
