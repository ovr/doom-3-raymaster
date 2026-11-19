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

void idEditorHistory::Undo( idEditorWorld & world ) {
	if ( !CanUndo() ) {
		return;
	}
	const step_t & step = steps[--position];
	for ( int i = step.changes.Num() - 1; i >= 0; i-- ) {
		world.SetMapArgs( step.changes[i].index, step.changes[i].before );
	}
}

void idEditorHistory::Redo( idEditorWorld & world ) {
	if ( !CanRedo() ) {
		return;
	}
	const step_t & step = steps[position++];
	for ( int i = 0; i < step.changes.Num(); i++ ) {
		world.SetMapArgs( step.changes[i].index, step.changes[i].after );
	}
}
