// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.
#pragma once

#include <memory>

class idEditorWorld;
struct editorPrimitiveChange_t;

// one entity's map key/values before and after an edit; none = the entity does not exist
struct editorChange_t {
	int					index = 0;
	idDict				before;
	idDict				after;
};

/*
	Undo and redo of the open map. An edit is a list of entity changes and of primitive changes
	that go back and forth together (one drag of three selected entities is one step; brushes
	made into an entity are the new entity's key/values and the primitives that move to it).
	Forward the entities change first, back the primitives.

	  steps:   [ move ] [ rotate ] [ move ] | [ undone ] [ undone ]
	                                        ▲ position; Undo() steps left, Redo() right,
	                                          a new edit drops what is right of it
*/
class idEditorHistory {
public:
	void				Clear();
	// what one step did, for the lists and the selection
	struct effect_t {
		bool				listChanged = false;		// see ChangesList()
		bool				primitivesChanged = false;
	};

	// the changes are already applied to the world
	void				Add( const char * label, const idList<editorChange_t> & changes );
	void				Add( const char * label, const idList<editorChange_t> & changes, const idList<editorPrimitiveChange_t> & primitiveChanges );

	[[nodiscard]] bool	CanUndo() const { return position > 0; }
	[[nodiscard]] bool	CanRedo() const { return position < steps.Num(); }
	// "" when there is nothing to undo or redo
	[[nodiscard]] const char *	UndoLabel() const { return CanUndo() ? steps[position - 1].label.c_str() : ""; }
	[[nodiscard]] const char *	RedoLabel() const { return CanRedo() ? steps[position].label.c_str() : ""; }
	effect_t			Undo( idEditorWorld & world );
	effect_t			Redo( idEditorWorld & world );
	// an entity appears, goes, or gets another name or class: lists of entities are out of date
	[[nodiscard]] static bool	ChangesList( const idList<editorChange_t> & changes );

	// the map differs from the file on disk
	[[nodiscard]] bool	IsDirty() const { return position != savedPosition; }
	void				MarkSaved() { savedPosition = position; }

private:
	struct step_t {
		idStr				label;
		idList<editorChange_t>	changes;
		// a pointer: the type is EditorWorld.h's, and a step is copied when the list grows
		std::shared_ptr<const idList<editorPrimitiveChange_t>>	primitiveChanges;
	};

	idList<step_t>		steps;
	int					position = 0;
	int					savedPosition = 0;	// -1: the saved state was dropped from the history
};
