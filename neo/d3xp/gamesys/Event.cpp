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
sys_event.cpp

Event are used for scheduling tasks and for linking script commands.

*/

#pragma hdrstop
#include "../../idlib/precompiled.h"


#include "../Game_local.h"

#define MAX_EVENTSPERFRAME			4096

/***********************************************************************

  idEventDef

***********************************************************************/

idEventDef *idEventDef::eventDefList[MAX_EVENTS];
int idEventDef::numEventDefs = 0;

static bool eventError = false;
static char eventErrorMsg[ 128 ];

// Bytes one argument takes in a queued event's data buffer, 0 for an invalid format char.
// A trace is stored as a valid flag, the trace_t and the material name.
static constexpr int EventArgDataSize( char format ) {
	switch ( format ) {
		case D_EVENT_FLOAT:			return sizeof( float );
		case D_EVENT_INTEGER:		return sizeof( int );
		case D_EVENT_VECTOR:		return sizeof( idVec3 );
		case D_EVENT_STRING:		return MAX_STRING_LEN;
		case D_EVENT_ENTITY:
		case D_EVENT_ENTITY_NULL:	return sizeof( idEntityPtr<idEntity> );
		case D_EVENT_TRACE:			return sizeof( bool ) + sizeof( trace_t ) + MAX_STRING_LEN;
		default:					return 0;
	}
}
static_assert( sizeof( idEntityPtr<idEntity> ) == sizeof( int ), "entity args are saved with WriteInt" );

/*
================
idEventDef::idEventDef
================
*/
idEventDef::idEventDef( const char *command, const char *formatspec, char returnType ) {
	idEventDef	*ev;
	int			i;
	unsigned int	bits;

	assert( command );
	assert( !idEvent::initialized );

	// Allow NULL to indicate no args, but always store it as ""
	// so we don't have to check for it.
	if ( !formatspec ) {
		formatspec = "";
	}

	this->name = command;
	this->formatspec = formatspec;
	this->returnType = returnType;

	numargs = idStr::Length( formatspec );
	assert( numargs <= D_EVENT_MAXARGS );
	if ( numargs > D_EVENT_MAXARGS ) {
		eventError = true;
		sprintf( eventErrorMsg, "idEventDef::idEventDef : Too many args for '%s' event.", name );
		return;
	}

	// make sure the format for the args is valid, calculate the formatspecindex, and the offsets for each arg
	bits = 0;
	argsize = 0;
	memset( argOffset, 0, sizeof( argOffset ) );
	for( i = 0; i < numargs;i++ ) {
		const int size = EventArgDataSize( formatspec[ i ] );
		if ( size == 0 ) {
			eventError = true;
			sprintf( eventErrorMsg, "idEventDef::idEventDef : Invalid arg format '%s' string for '%s' event.", formatspec, name );
			return;
		}
		if ( formatspec[ i ] == D_EVENT_FLOAT ) {
			bits |= 1 << i;
		}
		argOffset[ i ] = argsize;
		argsize += size;
	}

	// calculate the formatspecindex
	formatspecIndex = ( 1 << ( numargs + D_EVENT_MAXARGS ) ) | bits;

	// go through the list of defined events and check for duplicates
	// and mismatched format strings
	eventnum = numEventDefs;
	for( i = 0; i < eventnum; i++ ) {
		ev = eventDefList[ i ];
		if ( strcmp( command, ev->name ) == 0 ) {
			if ( strcmp( formatspec, ev->formatspec ) != 0 ) {
				eventError = true;
				sprintf( eventErrorMsg, "idEvent '%s' defined twice with same name but differing format strings ('%s'!='%s').",
					command, formatspec, ev->formatspec );
				return;
			}

			if ( ev->returnType != returnType ) {
				eventError = true;
				sprintf( eventErrorMsg, "idEvent '%s' defined twice with same name but differing return types ('%c'!='%c').",
					command, returnType, ev->returnType );
				return;
			}
			// Don't bother putting the duplicate event in list.
			eventnum = ev->eventnum;
			return;
		}
	}

	ev = this;

	if ( numEventDefs >= MAX_EVENTS ) {
		eventError = true;
		sprintf( eventErrorMsg, "numEventDefs >= MAX_EVENTS" );
		return;
	}
	eventDefList[numEventDefs] = ev;
	numEventDefs++;
}

/*
================
idEventDef::NumEventCommands
================
*/
int	idEventDef::NumEventCommands() {
	return numEventDefs;
}

/*
================
idEventDef::GetEventCommand
================
*/
const idEventDef *idEventDef::GetEventCommand( int eventnum ) {
	return eventDefList[ eventnum ];
}

/*
================
idEventDef::FindEvent
================
*/
const idEventDef *idEventDef::FindEvent( const char *name ) {
	idEventDef	*ev;
	int			num;
	int			i;

	assert( name );

	num = numEventDefs;
	for( i = 0; i < num; i++ ) {
		ev = eventDefList[ i ];
		if ( strcmp( name, ev->name ) == 0 ) {
			return ev;
		}
	}

	return NULL;
}

/***********************************************************************

  idEvent

***********************************************************************/

static idLinkList<idEvent> FreeEvents;
static idLinkList<idEvent> EventQueue;
static idLinkList<idEvent> FastEventQueue;
static idEvent EventPool[ MAX_EVENTS ];

bool idEvent::initialized = false;

idDynamicBlockAlloc<byte, 16 * 1024, 256>	idEvent::eventDataAllocator;

/*
================
idEvent::~idEvent()
================
*/
idEvent::~idEvent() {
	Free();
}

/*
================
idEvent::Alloc
================
*/
idEvent *idEvent::Alloc( const idEventDef *evdef, int numargs, va_list args ) {
	idEvent		*ev;
	int			size;
	const char	*format;
	idEventArg	*arg;
	byte		*dataPtr;
	int			i;
	const char	*materialName;

	if ( FreeEvents.IsListEmpty() ) {
		gameLocal.Error( "idEvent::Alloc : No more free events" );
	}

	ev = FreeEvents.Next();
	ev->eventNode.Remove();

	ev->eventdef = evdef;

	if ( numargs != evdef->GetNumArgs() ) {
		gameLocal.Error( "idEvent::Alloc : Wrong number of args for '%s' event.", evdef->GetName() );
	}

	size = evdef->GetArgSize();
	if ( size ) {
		ev->data = eventDataAllocator.Alloc( size );
		memset( ev->data, 0, size );
	} else {
		ev->data = NULL;
		return ev;
	}

	format = evdef->GetArgFormat();
	for( i = 0; i < numargs; i++ ) {
		arg = va_arg( args, idEventArg * );
		if ( format[ i ] != arg->type ) {
			// when NULL is passed in for an entity, it gets cast as an integer 0, so don't give an error when it happens
			if ( !( ( ( format[ i ] == D_EVENT_TRACE ) || ( format[ i ] == D_EVENT_ENTITY ) ) && ( arg->type == 'd' ) && ( arg->value == 0 ) ) ) {
				gameLocal.Error( "idEvent::Alloc : Wrong type passed in for arg # %d on '%s' event.", i, evdef->GetName() );
			}
		}

		dataPtr = &ev->data[ evdef->GetArgOffset( i ) ];

		switch( format[ i ] ) {
		case D_EVENT_FLOAT :
		case D_EVENT_INTEGER :
			// a float arg keeps its bits in the low 32 bits of the slot
			*reinterpret_cast<int *>( dataPtr ) = static_cast<int>( arg->value );
			break;

		case D_EVENT_VECTOR :
			if ( arg->value ) {
				*reinterpret_cast<idVec3 *>( dataPtr ) = *reinterpret_cast<const idVec3 *>( arg->value );
			}
			break;

		case D_EVENT_STRING :
			if ( arg->value ) {
				idStr::Copynz( reinterpret_cast<char *>( dataPtr ), reinterpret_cast<const char *>( arg->value ), MAX_STRING_LEN );
			}
			break;

		case D_EVENT_ENTITY :
		case D_EVENT_ENTITY_NULL :
			*reinterpret_cast< idEntityPtr<idEntity> * >( dataPtr ) = reinterpret_cast<idEntity *>( arg->value );
			break;

		case D_EVENT_TRACE :
			if ( arg->value ) {
				*reinterpret_cast<bool *>( dataPtr ) = true;
				*reinterpret_cast<trace_t *>( dataPtr + sizeof( bool ) ) = *reinterpret_cast<const trace_t *>( arg->value );

				// save off the material as a string since the pointer won't be valid in save games.
				// since we save off the entire trace_t structure, if the material is NULL here,
				// it will be NULL when we process it, so we don't need to save off anything in that case.
				if ( reinterpret_cast<const trace_t *>( arg->value )->c.material ) {
					materialName = reinterpret_cast<const trace_t *>( arg->value )->c.material->GetName();
					idStr::Copynz( reinterpret_cast<char *>( dataPtr + sizeof( bool ) + sizeof( trace_t ) ), materialName, MAX_STRING_LEN );
				}
			} else {
				*reinterpret_cast<bool *>( dataPtr ) = false;
			}
			break;

		default :
			gameLocal.Error( "idEvent::Alloc : Invalid arg format '%s' string for '%s' event.", format, evdef->GetName() );
			break;
		}
	}

	return ev;
}

/*
================
idEvent::CopyArgs
================
*/
void idEvent::CopyArgs( const idEventDef *evdef, int numargs, va_list args, eventArg_t data[ D_EVENT_MAXARGS ] ) {
	int			i;
	const char	*format;
	idEventArg	*arg;

	format = evdef->GetArgFormat();
	if ( numargs != evdef->GetNumArgs() ) {
		gameLocal.Error( "idEvent::CopyArgs : Wrong number of args for '%s' event.", evdef->GetName() );
	}

	for( i = 0; i < numargs; i++ ) {
		arg = va_arg( args, idEventArg * );
		if ( format[ i ] != arg->type ) {
			// when NULL is passed in for an entity, it gets cast as an integer 0, so don't give an error when it happens
			if ( !( ( ( format[ i ] == D_EVENT_TRACE ) || ( format[ i ] == D_EVENT_ENTITY ) ) && ( arg->type == 'd' ) && ( arg->value == 0 ) ) ) {
				gameLocal.Error( "idEvent::CopyArgs : Wrong type passed in for arg # %d on '%s' event.", i, evdef->GetName() );
			}
		}

		data[ i ] = arg->value;
	}
}

/*
================
idEvent::Free
================
*/
void idEvent::Free() {
	if ( data ) {
		eventDataAllocator.Free( data );
		data = NULL;
	}

	eventdef	= NULL;
	time		= 0;
	object		= NULL;
	typeinfo	= NULL;

	eventNode.SetOwner( this );
	eventNode.AddToEnd( FreeEvents );
}

/*
================
idEvent::Schedule
================
*/
void idEvent::Schedule( idClass *obj, const idTypeInfo *type, int time ) {
	idEvent *event;

	assert( initialized );
	if ( !initialized ) {
		return;
	}

	object = obj;
	typeinfo = type;

	// wraps after 24 days...like I care. ;)
	this->time = gameLocal.time + time;

	eventNode.Remove();

	if ( obj->IsType( idEntity::Type ) && ( ( (idEntity*)(obj) )->timeGroup == TIME_GROUP2 ) ) {
		event = FastEventQueue.Next();
		while( ( event != NULL ) && ( this->time >= event->time ) ) {
			event = event->eventNode.Next();
		}

		if ( event ) {
			eventNode.InsertBefore( event->eventNode );
		} else {
			eventNode.AddToEnd( FastEventQueue );
		}

		return;
	} else {
		this->time = gameLocal.slow.time + time;
	}

	event = EventQueue.Next();
	while( ( event != NULL ) && ( this->time >= event->time ) ) {
		event = event->eventNode.Next();
	}

	if ( event ) {
		eventNode.InsertBefore( event->eventNode );
	} else {
		eventNode.AddToEnd( EventQueue );
	}
}

/*
================
idEvent::CancelEvents
================
*/
void idEvent::CancelEvents( const idClass *obj, const idEventDef *evdef ) {
	idEvent *event;
	idEvent *next;

	if ( !initialized ) {
		return;
	}

	for( event = EventQueue.Next(); event != NULL; event = next ) {
		next = event->eventNode.Next();
		if ( event->object == obj ) {
			if ( !evdef || ( evdef == event->eventdef ) ) {
				event->Free();
			}
		}
	}

	for( event = FastEventQueue.Next(); event != NULL; event = next ) {
		next = event->eventNode.Next();
		if ( event->object == obj ) {
			if ( !evdef || ( evdef == event->eventdef ) ) {
				event->Free();
			}
		}
	}
}

/*
================
idEvent::ClearEventList
================
*/
void idEvent::ClearEventList() {
	int i;

	//
	// initialize lists
	//
	FreeEvents.Clear();
	EventQueue.Clear();
   
	// 
	// add the events to the free list
	//
	for( i = 0; i < MAX_EVENTS; i++ ) {
		EventPool[ i ].Free();
	}
}

// Turns the queued data into handler args: values for ints and floats, pointers into the data
// buffer for everything else.
void idEvent::GetArgs( eventArg_t args[ D_EVENT_MAXARGS ] ) {
	const char *formatspec = eventdef->GetArgFormat();
	const int numargs = eventdef->GetNumArgs();

	for ( int i = 0; i < numargs; i++ ) {
		byte *arg = &data[ eventdef->GetArgOffset( i ) ];
		switch( formatspec[ i ] ) {
		case D_EVENT_FLOAT :
			args[ i ] = EventArgFromFloat( *reinterpret_cast<const float *>( arg ) );
			break;

		case D_EVENT_INTEGER :
			args[ i ] = *reinterpret_cast<const int *>( arg );
			break;

		case D_EVENT_VECTOR :
		case D_EVENT_STRING :
			args[ i ] = reinterpret_cast<eventArg_t>( arg );
			break;

		case D_EVENT_ENTITY :
		case D_EVENT_ENTITY_NULL :
			args[ i ] = reinterpret_cast<eventArg_t>( reinterpret_cast<const idEntityPtr<idEntity> *>( arg )->GetEntity() );
			break;

		case D_EVENT_TRACE :
			if ( *reinterpret_cast<const bool *>( arg ) ) {
				trace_t *trace = reinterpret_cast<trace_t *>( arg + sizeof( bool ) );
				if ( trace->c.material != nullptr ) {
					// the material is looked up by name, the stored pointer may come from a save game
					const char *materialName = reinterpret_cast<const char *>( arg + sizeof( bool ) + sizeof( trace_t ) );
					trace->c.material = declManager->FindMaterial( materialName, true );
				}
				args[ i ] = reinterpret_cast<eventArg_t>( trace );
			} else {
				args[ i ] = 0;
			}
			break;

		default:
			gameLocal.Error( "idEvent::GetArgs : Invalid arg format '%s' string for '%s' event.", formatspec, eventdef->GetName() );
		}
	}
}

/*
================
idEvent::ServiceEvents
================
*/
void idEvent::ServiceEvents() {
	idEvent		*event;
	int			num;
	eventArg_t	args[ D_EVENT_MAXARGS ];
	const idEventDef *ev;

	num = 0;
	while( !EventQueue.IsListEmpty() ) {
		event = EventQueue.Next();
		assert( event );

		if ( event->time > gameLocal.time ) {
			break;
		}

		common->UpdateLevelLoadPacifier();

		ev = event->eventdef;
		event->GetArgs( args );

		// the event is removed from its list so that if then object
		// is deleted, the event won't be freed twice
		event->eventNode.Remove();
		assert( event->object );
		event->object->ProcessEventArgPtr( ev, args );

		// return the event to the free list
		event->Free();

		// Don't allow ourselves to stay in here too long.  An abnormally high number
		// of events being processed is evidence of an infinite loop of events.
		num++;
		if ( num > MAX_EVENTSPERFRAME ) {
			gameLocal.Error( "Event overflow.  Possible infinite loop in script." );
		}
	}
}

/*
================
idEvent::ServiceFastEvents
================
*/
void idEvent::ServiceFastEvents() {
	idEvent		*event;
	int			num;
	eventArg_t	args[ D_EVENT_MAXARGS ];
	const idEventDef *ev;

	num = 0;
	while( !FastEventQueue.IsListEmpty() ) {
		event = FastEventQueue.Next();
		assert( event );

		if ( event->time > gameLocal.fast.time ) {
			break;
		}

		ev = event->eventdef;
		event->GetArgs( args );

		// the event is removed from its list so that if then object
		// is deleted, the event won't be freed twice
		event->eventNode.Remove();
		assert( event->object );
		event->object->ProcessEventArgPtr( ev, args );

		// return the event to the free list
		event->Free();

		// Don't allow ourselves to stay in here too long.  An abnormally high number
		// of events being processed is evidence of an infinite loop of events.
		num++;
		if ( num > MAX_EVENTSPERFRAME ) {
			gameLocal.Error( "Event overflow.  Possible infinite loop in script." );
		}
	}
}

/*
================
idEvent::Init
================
*/
void idEvent::Init() {
	gameLocal.Printf( "Initializing event system\n" );

	if ( eventError ) {
		gameLocal.Error( "%s", eventErrorMsg );
	}

	if ( initialized ) {
		gameLocal.Printf( "...already initialized\n" );
		ClearEventList();
		return;
	}

	ClearEventList();

	eventDataAllocator.Init();

	gameLocal.Printf( "...%i event definitions\n", idEventDef::NumEventCommands() );

	// the event system has started
	initialized = true;
}

/*
================
idEvent::Shutdown
================
*/
void idEvent::Shutdown() {
	gameLocal.Printf( "Shutdown event system\n" );

	if ( !initialized ) {
		gameLocal.Printf( "...not started\n" );
		return;
	}

	ClearEventList();
	
	eventDataAllocator.Shutdown();

	// say it is now shutdown
	initialized = false;
}

void idEvent::SaveArgs( idSaveGame *savefile ) const {
	const char *format = eventdef->GetArgFormat();

	savefile->WriteInt( eventdef->GetArgSize() );
	for ( int i = 0; i < eventdef->GetNumArgs(); i++ ) {
		const byte *arg = &data[ eventdef->GetArgOffset( i ) ];
		switch( format[ i ] ) {
			case D_EVENT_FLOAT :
				savefile->WriteFloat( *reinterpret_cast<const float *>( arg ) );
				break;
			case D_EVENT_INTEGER :
			case D_EVENT_ENTITY :
			case D_EVENT_ENTITY_NULL :
				savefile->WriteInt( *reinterpret_cast<const int *>( arg ) );
				break;
			case D_EVENT_VECTOR :
				savefile->WriteVec3( *reinterpret_cast<const idVec3 *>( arg ) );
				break;
			case D_EVENT_STRING :
				savefile->WriteString( reinterpret_cast<const char *>( arg ) );
				break;
			case D_EVENT_TRACE : {
				const bool validTrace = *reinterpret_cast<const bool *>( arg );
				savefile->WriteBool( validTrace );
				if ( validTrace ) {
					// the material goes by name, the stored pointer is meaningless in a save game
					savefile->WriteTrace( *reinterpret_cast<const trace_t *>( arg + sizeof( bool ) ) );
				}
				break;
			}
			default:
				break;
		}
	}
}

void idEvent::RestoreArgs( idRestoreGame *savefile ) {
	int argsize;

	savefile->ReadInt( argsize );
	if ( argsize != eventdef->GetArgSize() ) {
		savefile->Error( "idEvent::Restore: arg size (%d) doesn't match saved arg size(%d) on event '%s'", eventdef->GetArgSize(), argsize, eventdef->GetName() );
	}
	if ( argsize == 0 ) {
		data = nullptr;
		return;
	}

	data = eventDataAllocator.Alloc( argsize );
	memset( data, 0, argsize );

	const char *format = eventdef->GetArgFormat();
	idStr str;
	for ( int i = 0; i < eventdef->GetNumArgs(); i++ ) {
		byte *arg = &data[ eventdef->GetArgOffset( i ) ];
		switch( format[ i ] ) {
			case D_EVENT_FLOAT :
				savefile->ReadFloat( *reinterpret_cast<float *>( arg ) );
				break;
			case D_EVENT_INTEGER :
			case D_EVENT_ENTITY :
			case D_EVENT_ENTITY_NULL :
				savefile->ReadInt( *reinterpret_cast<int *>( arg ) );
				break;
			case D_EVENT_VECTOR :
				savefile->ReadVec3( *reinterpret_cast<idVec3 *>( arg ) );
				break;
			case D_EVENT_STRING :
				savefile->ReadString( str );
				idStr::Copynz( reinterpret_cast<char *>( arg ), str, MAX_STRING_LEN );
				break;
			case D_EVENT_TRACE : {
				bool &validTrace = *reinterpret_cast<bool *>( arg );
				savefile->ReadBool( validTrace );
				if ( validTrace ) {
					trace_t &trace = *reinterpret_cast<trace_t *>( arg + sizeof( bool ) );
					savefile->ReadTrace( trace );
					// GetArgs looks the material up by the name stored behind the trace
					if ( trace.c.material != nullptr ) {
						idStr::Copynz( reinterpret_cast<char *>( arg + sizeof( bool ) + sizeof( trace_t ) ), trace.c.material->GetName(), MAX_STRING_LEN );
					}
				}
				break;
			}
			default:
				break;
		}
	}
}

/*
================
idEvent::Save
================
*/
void idEvent::Save( idSaveGame *savefile ) {
	for ( idLinkList<idEvent> *queue : { &EventQueue, &FastEventQueue } ) {
		savefile->WriteInt( queue->Num() );
		for ( const idEvent *event = queue->Next(); event != nullptr; event = event->eventNode.Next() ) {
			savefile->WriteInt( event->time );
			savefile->WriteString( event->eventdef->GetName() );
			savefile->WriteString( event->typeinfo->classname );
			savefile->WriteObject( event->object );
			event->SaveArgs( savefile );
		}
	}
}

/*
================
idEvent::Restore
================
*/
void idEvent::Restore( idRestoreGame *savefile ) {
	idStr name;

	for ( idLinkList<idEvent> *queue : { &EventQueue, &FastEventQueue } ) {
		int num;
		savefile->ReadInt( num );

		for ( int i = 0; i < num; i++ ) {
			if ( FreeEvents.IsListEmpty() ) {
				gameLocal.Error( "idEvent::Restore : No more free events" );
			}

			idEvent *event = FreeEvents.Next();
			event->eventNode.Remove();
			event->eventNode.AddToEnd( *queue );

			savefile->ReadInt( event->time );

			// read the event name
			savefile->ReadString( name );
			event->eventdef = idEventDef::FindEvent( name );
			if ( event->eventdef == nullptr ) {
				savefile->Error( "idEvent::Restore: unknown event '%s'", name.c_str() );
				return;
			}

			// read the classtype
			savefile->ReadString( name );
			event->typeinfo = idClass::GetClass( name );
			if ( event->typeinfo == nullptr ) {
				savefile->Error( "idEvent::Restore: unknown class '%s' on event '%s'", name.c_str(), event->eventdef->GetName() );
				return;
			}

			savefile->ReadObject( event->object );
			event->RestoreArgs( savefile );
		}
	}
}
