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
	Add( label, changes, idList<editorPrimitiveChange_t>() );
}

void idEditorHistory::Add( const char * label, const idList<editorChange_t> & changes, const idList<editorPrimitiveChange_t> & primitiveChanges ) {
	if ( changes.Num() == 0 && primitiveChanges.Num() == 0 ) {
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
	if ( primitiveChanges.Num() > 0 ) {
		step.primitiveChanges = std::make_shared<const idList<editorPrimitiveChange_t>>( primitiveChanges );
	}
	position = steps.Num();
}

idEditorHistory::effect_t idEditorHistory::Undo( idEditorWorld & world ) {
	if ( !CanUndo() ) {
		return {};
	}
	const step_t & step = steps[--position];
	if ( step.primitiveChanges != nullptr ) {
		for ( int i = step.primitiveChanges->Num() - 1; i >= 0; i-- ) {
			world.ApplyPrimitive( ( *step.primitiveChanges )[i], false );
		}
	}
	for ( int i = step.changes.Num() - 1; i >= 0; i-- ) {
		world.SetMapArgs( step.changes[i].index, step.changes[i].before );
	}
	return { ChangesList( step.changes ), step.primitiveChanges != nullptr };
}

idEditorHistory::effect_t idEditorHistory::Redo( idEditorWorld & world ) {
	if ( !CanRedo() ) {
		return {};
	}
	const step_t & step = steps[position++];
	for ( int i = 0; i < step.changes.Num(); i++ ) {
		world.SetMapArgs( step.changes[i].index, step.changes[i].after );
	}
	if ( step.primitiveChanges != nullptr ) {
		for ( int i = 0; i < step.primitiveChanges->Num(); i++ ) {
			world.ApplyPrimitive( ( *step.primitiveChanges )[i], true );
		}
	}
	return { ChangesList( step.changes ), step.primitiveChanges != nullptr };
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
