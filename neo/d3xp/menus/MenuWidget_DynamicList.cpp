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
#pragma hdrstop
#include "../../idLib/precompiled.h"
#include "../Game_local.h"

/*
========================
idMenuWidget_DynamicList::Initialize
========================
*/
void idMenuWidget_DynamicList::Initialize( idMenuHandler * data ) {
	idMenuWidget::Initialize( data );
}

/*
========================
idMenuWidget_DynamicList::Update
========================
*/
void idMenuWidget_DynamicList::Update() {

	if ( GetSWFObject() == NULL ) {
		return;
	}

	idSWFScriptObject & root = GetSWFObject()->GetRootObject();

	if ( !BindSprite( root ) ) {
		return;
	}

	if ( controlList ) {
		UpdateControlList( root );
		return;
	}

	for ( int optionIndex = 0; optionIndex < GetNumVisibleOptions(); ++optionIndex ) {

		if ( optionIndex >= children.Num() ) {
			idSWFSpriteInstance * item = GetSprite()->GetScriptObject()->GetNestedSprite( va( "item%d", optionIndex ) );
			if ( item != NULL ) {
				item->SetVisible( false );
				continue;
			}
		}

		idMenuWidget & child = GetChildByIndex( optionIndex );
		const int childIndex = GetViewOffset() + optionIndex;
		bool shown = false;
		child.SetSpritePath( GetSpritePath(), va( "item%d", optionIndex ) );
		if ( child.BindSprite( root ) ) {
			
			if ( optionIndex >= GetTotalNumberOfOptions() ) {
				child.ClearSprite();
				continue;
			} else {
				//const int controlIndex = GetNumVisibleOptions() - Min( GetNumVisibleOptions(), GetTotalNumberOfOptions() ) + optionIndex;
				shown = PrepareListElement( child, childIndex );
				child.Update();
			}

			if ( !shown ) {
				child.SetState( WIDGET_STATE_HIDDEN );
			} else {
				if ( optionIndex == focusIndex ) {
					child.SetState( WIDGET_STATE_SELECTING );
				} else {
					child.SetState( WIDGET_STATE_NORMAL );
				}
			}
		}
	}

	idSWFSpriteInstance * const upSprite = GetSprite()->GetScriptObject()->GetSprite( "upIndicator" );
	if ( upSprite != NULL ) {
		upSprite->SetVisible( GetViewOffset() > 0 );
	}

	idSWFSpriteInstance * const downSprite = GetSprite()->GetScriptObject()->GetSprite( "downIndicator" );
	if ( downSprite != NULL ) {
		downSprite->SetVisible( GetViewOffset() + GetNumVisibleOptions() < GetTotalNumberOfOptions() );
	}

}

/*
========================
idMenuWidget_DynamicList::UpdateControlList

A control list owns one widget per option. Only the controls inside the
view window are bound to the item sprites, so a list with more controls
than item sprites scrolls.
========================
*/
void idMenuWidget_DynamicList::UpdateControlList( idSWFScriptObject & root ) {
	const int viewOffset = GetViewOffset();
	const int numVisible = GetNumVisibleOptions();

	// unbind the controls scrolled out of view
	for ( int childIndex = 0; childIndex < children.Num(); ++childIndex ) {
		const int slot = childIndex - viewOffset;
		if ( slot < 0 || slot >= numVisible ) {
			GetChildByIndex( childIndex ).ClearSprite();
		}
	}

	for ( int optionIndex = 0; optionIndex < numVisible; ++optionIndex ) {
		const int childIndex = viewOffset + optionIndex;

		if ( childIndex >= children.Num() ) {
			idSWFSpriteInstance * item = GetSprite()->GetScriptObject()->GetNestedSprite( va( "item%d", optionIndex ) );
			if ( item != NULL ) {
				item->SetVisible( false );
			}
			continue;
		}

		idMenuWidget & child = GetChildByIndex( childIndex );
		child.SetSpritePath( GetSpritePath(), va( "item%d", optionIndex ) );
		if ( !child.BindSprite( root ) ) {
			continue;
		}
		child.GetSprite()->SetVisible( true );

		const bool shown = PrepareListElement( child, childIndex );
		child.Update();

		if ( !shown ) {
			child.SetState( WIDGET_STATE_HIDDEN );
		} else if ( optionIndex == focusIndex ) {
			child.SetState( WIDGET_STATE_SELECTING );
		} else {
			child.SetState( WIDGET_STATE_NORMAL );
		}
	}

	idSWFSpriteInstance * const upSprite = GetSprite()->GetScriptObject()->GetSprite( "upIndicator" );
	if ( upSprite != NULL ) {
		upSprite->SetVisible( viewOffset > 0 );
	}

	idSWFSpriteInstance * const downSprite = GetSprite()->GetScriptObject()->GetSprite( "downIndicator" );
	if ( downSprite != NULL ) {
		downSprite->SetVisible( viewOffset + numVisible < GetTotalNumberOfOptions() );
	}
}

/*
========================
idMenuWidget_DynamicList::GetFocus
========================
*/
idMenuWidget * idMenuWidget_DynamicList::GetFocus() {
	if ( !controlList ) {
		return idMenuWidget_List::GetFocus();
	}
	const int childIndex = GetViewOffset() + focusIndex;
	if ( focusIndex < 0 || childIndex < 0 || childIndex >= children.Num() ) {
		return NULL;
	}
	return children[ childIndex ];
}

/*
========================
idMenuWidget_DynamicList::SetFocusIndex

For a control list the index is a visible slot, the control in it is viewOffset + index.
========================
*/
void idMenuWidget_DynamicList::SetFocusIndex( const int index, bool skipSound ) {
	if ( !controlList ) {
		idMenuWidget_List::SetFocusIndex( index, skipSound );
		return;
	}

	const int oldIndex = focusIndex;
	focusIndex = index;

	if ( oldIndex != focusIndex && !skipSound ) {
		if ( menuData != NULL ) {
			menuData->PlaySound( GUI_SOUND_FOCUS );
		}
	}

	idSWFParmList parms;
	parms.Append( oldIndex );
	parms.Append( index );

	// the view offset may have changed together with the focus, so look at
	// the state of every control instead of only the old and new slot
	const int viewOffset = GetViewOffset();
	for ( int childIndex = 0; childIndex < children.Num(); ++childIndex ) {
		idMenuWidget & child = GetChildByIndex( childIndex );
		const widgetState_t state = child.GetState();
		if ( state == WIDGET_STATE_HIDDEN || state == WIDGET_STATE_DISABLED ) {
			continue;
		}
		const bool focused = ( childIndex - viewOffset ) == focusIndex;
		if ( focused && state != WIDGET_STATE_SELECTING ) {
			child.ReceiveEvent( idWidgetEvent( WIDGET_EVENT_FOCUS_ON, 0, NULL, parms ) );
		} else if ( !focused && state == WIDGET_STATE_SELECTING ) {
			child.ReceiveEvent( idWidgetEvent( WIDGET_EVENT_FOCUS_OFF, 0, NULL, parms ) );
		}
	}
}

/*
========================
idMenuWidget_DynamicList::GetTotalNumberOfOptions
========================
*/
int idMenuWidget_DynamicList::GetTotalNumberOfOptions() const {

	if ( controlList ) {
		return GetChildren().Num();
	}

	return listItemInfo.Num();
}

/*
========================
idMenuWidget_DynamicList::PrepareListElement
========================
*/
bool idMenuWidget_DynamicList::PrepareListElement( idMenuWidget & widget, const int childIndex ) {

	idMenuWidget_ScoreboardButton * const sbButton = dynamic_cast< idMenuWidget_ScoreboardButton * >( &widget );
	if ( sbButton != NULL ) {
		return true;
	}

	if ( listItemInfo.Num() == 0 ) {
		return true;
	}

	if ( childIndex > listItemInfo.Num() ) {
		return false;
	}

	idMenuWidget_Button * const button = dynamic_cast< idMenuWidget_Button * >( &widget );
	if ( button != NULL ) {
		button->SetIgnoreColor( ignoreColor );
		button->SetValues( listItemInfo[ childIndex ] );
		if ( listItemInfo[ childIndex ].Num() > 0 ) {
			return true;
		} 
	}
	
	return false;
}

/*
========================
idMenuWidget_DynamicList::SetListData
========================
*/
void idMenuWidget_DynamicList::SetListData( idList< idList< idStr, TAG_IDLIB_LIST_MENU >, TAG_IDLIB_LIST_MENU > & list ) {
	listItemInfo.Clear();
	for ( int i = 0; i < list.Num(); ++i ) {
		idList< idStr > values;
		for ( int j = 0; j < list[i].Num(); ++j ) {
			values.Append( list[i][j] );
		}
		listItemInfo.Append( values );
	}
}

/*
========================
idMenuWidget_DynamicList::Recalculate
========================
*/
void idMenuWidget_DynamicList::Recalculate() {

	idSWF * swf = GetSWFObject();

	if ( swf == NULL ) {
		return;
	}

	idSWFScriptObject & root = swf->GetRootObject();
	for ( int i = 0; i < GetChildren().Num(); ++i ) {
		idMenuWidget & child = GetChildByIndex( i );
		child.SetSpritePath( GetSpritePath(), "info", "list", va( "item%d", i ) );
		if ( child.BindSprite( root ) ) {
			child.SetState( WIDGET_STATE_NORMAL );
			child.GetSprite()->StopFrame( 1 );
		}
	}
}

/*
========================
idMenuWidget_ScoreboardList::Update
========================
*/
void idMenuWidget_ScoreboardList::Update() {

	if ( GetSWFObject() == NULL ) {
		return;
	}

	idSWFScriptObject & root = GetSWFObject()->GetRootObject();

	if ( !BindSprite( root ) ) {
		return;
	}

	for ( int optionIndex = 0; optionIndex < GetNumVisibleOptions(); ++optionIndex ) {
		idMenuWidget & child = GetChildByIndex( optionIndex );
		const int childIndex = GetViewOffset() + optionIndex;
		bool shown = false;
		child.SetSpritePath( GetSpritePath(), va( "item%d", optionIndex ) );
		if ( child.BindSprite( root ) ) {
			shown = PrepareListElement( child, childIndex );

			if ( optionIndex == focusIndex && child.GetState() != WIDGET_STATE_SELECTED && child.GetState() != WIDGET_STATE_SELECTING ) {
				child.SetState( WIDGET_STATE_SELECTING );
			} else if ( optionIndex != focusIndex && child.GetState() != WIDGET_STATE_NORMAL ) {
				child.SetState( WIDGET_STATE_NORMAL );
			}

			child.Update();
		}
	}
}

/*
========================
idMenuWidget_ScoreboardList::GetTotalNumberOfOptions
========================
*/
int idMenuWidget_ScoreboardList::GetTotalNumberOfOptions() const {
	return GetChildren().Num();
}