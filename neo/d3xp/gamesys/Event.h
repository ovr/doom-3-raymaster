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
/*
sys_event.h

Event are used for scheduling tasks and for linking script commands.
*/
#ifndef __SYS_EVENT_H__
#define __SYS_EVENT_H__

#include <bit>

#define D_EVENT_MAXARGS				8			// idClass::ProcessEventArgPtr has a dispatch case per arg count

#define D_EVENT_VOID				( ( char )0 )
#define D_EVENT_INTEGER				'd'
#define D_EVENT_FLOAT				'f'
#define D_EVENT_VECTOR				'v'
#define D_EVENT_STRING				's'
#define D_EVENT_ENTITY				'e'
#define	D_EVENT_ENTITY_NULL			'E'			// event can handle NULL entity pointers
#define D_EVENT_TRACE				't'

#define MAX_EVENTS					4096

// One argument as handed to an event handler: an int, a pointer, or a float's bits in the low
// 32 bits. Pointer-sized, so each slot maps onto one register or stack slot of the Win64 call.
using eventArg_t = intptr_t;

ID_INLINE eventArg_t EventArgFromFloat( float value ) {
	return static_cast<eventArg_t>( std::bit_cast<uint32_t>( value ) );
}

ID_INLINE float EventArgToFloat( eventArg_t arg ) {
	return std::bit_cast<float>( static_cast<uint32_t>( arg ) );
}

class idClass;
class idTypeInfo;

class idEventDef {
private:
	const char					*name;
	const char					*formatspec;
	unsigned int				formatspecIndex;
	int							returnType;
	int							numargs;
	int							argsize;
	int							argOffset[ D_EVENT_MAXARGS ];
	int							eventnum;
	const idEventDef *			next;

	static idEventDef *			eventDefList[MAX_EVENTS];
	static int					numEventDefs;

public:
								idEventDef( const char *command, const char *formatspec = NULL, char returnType = 0 );
								
	const char					*GetName() const;
	const char					*GetArgFormat() const;
	unsigned int				GetFormatspecIndex() const;
	char						GetReturnType() const;
	int							GetEventNum() const;
	int							GetNumArgs() const;
	int							GetArgSize() const;
	int							GetArgOffset( int arg ) const;

	static int					NumEventCommands();
	static const idEventDef		*GetEventCommand( int eventnum );
	static const idEventDef		*FindEvent( const char *name );
};

class idSaveGame;
class idRestoreGame;

class idEvent {
private:
	const idEventDef			*eventdef;
	byte						*data;
	int							time;
	idClass						*object;
	const idTypeInfo			*typeinfo;

	idLinkList<idEvent>			eventNode;

	static idDynamicBlockAlloc<byte, 16 * 1024, 256> eventDataAllocator;

	void						GetArgs( eventArg_t args[ D_EVENT_MAXARGS ] );
	void						SaveArgs( idSaveGame *savefile ) const;
	void						RestoreArgs( idRestoreGame *savefile );

public:
	static bool					initialized;

								~idEvent();

	static idEvent				*Alloc( const idEventDef *evdef, int numargs, va_list args );
	static void					CopyArgs( const idEventDef *evdef, int numargs, va_list args, eventArg_t data[ D_EVENT_MAXARGS ] );

	void						Free();
	void						Schedule( idClass *object, const idTypeInfo *cls, int time );
	byte						*GetData();

	static void					CancelEvents( const idClass *obj, const idEventDef *evdef = NULL );
	static void					ClearEventList();
	static void					ServiceEvents();
	static void					ServiceFastEvents();
	static void					Init();
	static void					Shutdown();

	// save games
	static void					Save( idSaveGame *savefile );					// archives object for save game file
	static void					Restore( idRestoreGame *savefile );				// unarchives object from save game file
};

/*
================
idEvent::GetData
================
*/
ID_INLINE byte *idEvent::GetData() {
	return data;
}

/*
================
idEventDef::GetName
================
*/
ID_INLINE const char *idEventDef::GetName() const {
	return name;
}

/*
================
idEventDef::GetArgFormat
================
*/
ID_INLINE const char *idEventDef::GetArgFormat() const {
	return formatspec;
}

/*
================
idEventDef::GetFormatspecIndex
================
*/
ID_INLINE unsigned int idEventDef::GetFormatspecIndex() const {
	return formatspecIndex;
}

/*
================
idEventDef::GetReturnType
================
*/
ID_INLINE char idEventDef::GetReturnType() const {
	return returnType;
}

/*
================
idEventDef::GetNumArgs
================
*/
ID_INLINE int idEventDef::GetNumArgs() const {
	return numargs;
}

/*
================
idEventDef::GetArgSize
================
*/
ID_INLINE int idEventDef::GetArgSize() const {
	return argsize;
}

/*
================
idEventDef::GetArgOffset
================
*/
ID_INLINE int idEventDef::GetArgOffset( int arg ) const {
	assert( ( arg >= 0 ) && ( arg < D_EVENT_MAXARGS ) );
	return argOffset[ arg ];
}

/*
================
idEventDef::GetEventNum
================
*/
ID_INLINE int idEventDef::GetEventNum() const {
	return eventnum;
}

#endif /* !__SYS_EVENT_H__ */
