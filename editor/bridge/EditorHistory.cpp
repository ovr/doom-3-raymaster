// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "idlib/precompiled.h"

#include "EditorHistory.h"
#include "EditorWorld.h"

void idEditorHistory::Clear() {
	steps.Clear();
	position = 0;
	savedPosition = 0;
}

void idEditorHistory::Add( const char * label, const idList<editorChange_t> & changes ) {
	if ( changes.Num() == 0 ) {
		return;
	}
	// the undone steps are gone, and with them maybe the state that is on disk
	if ( savedPosition > position ) {
		savedPosition = -1;
	}
	steps.SetNum( position );

	step_t & step = steps.Alloc();
	step.label = label;
	step.changes = changes;
	position = steps.Num();
}

bool idEditorHistory::Undo( idEditorWorld & world ) {
	if ( !CanUndo() ) {
		return false;
	}
	const step_t & step = steps[--position];
	for ( int i = step.changes.Num() - 1; i >= 0; i-- ) {
		world.SetMapArgs( step.changes[i].index, step.changes[i].before );
	}
	return ChangesList( step.changes );
}

bool idEditorHistory::Redo( idEditorWorld & world ) {
	if ( !CanRedo() ) {
		return false;
	}
	const step_t & step = steps[position++];
	for ( int i = 0; i < step.changes.Num(); i++ ) {
		world.SetMapArgs( step.changes[i].index, step.changes[i].after );
	}
	return ChangesList( step.changes );
}

bool idEditorHistory::ChangesList( const idList<editorChange_t> & changes ) {
	for ( int i = 0; i < changes.Num(); i++ ) {
		const idDict & before = changes[i].before;
		const idDict & after = changes[i].after;
		if ( idStr::Cmp( before.GetString( "name" ), after.GetString( "name" ) ) != 0
				|| idStr::Cmp( before.GetString( "classname" ), after.GetString( "classname" ) ) != 0
				|| ( before.GetNumKeyVals() == 0 ) != ( after.GetNumKeyVals() == 0 ) ) {
			return true;
		}
	}
	return false;
}
