// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.
#pragma once

/*
	Pictures of materials, models and entity classes, for thumbnails and the material editor.

	A picture is a frame of its own that nobody sees:

	  Request() ── queue ── Draw()      in place of the edit view: a world of one model and two
	                          │         lights, drawn into a corner of the output
	                        the engine draws the frame
	                          │
	                        Finish()    reads the corner back; the frame is never presented, the
	                                    screen keeps the frame before

	So a picture costs one frame of the edit view, and its size is limited by the viewport's.
	A material is shown on a sphere.
*/
class idEditorPreview {
public:
	struct job_t {
		int					id = 0;
		int					kind = 0;		// editorPreviewKind_t
		idStr				name;
		int					size = 0;		// the picture is size x size
	};

						~idEditorPreview() { Shutdown(); }
	void				Shutdown();

	void				Request( const job_t & job ) { queue.Append( job ); }
	void				Cancel() { queue.Clear(); }
	[[nodiscard]] bool	HasPending() const { return queue.Num() > 0; }

	// The first job when its frame ended in an engine error, so Finish() never came: it is taken
	// off the queue, or it would be tried, and fail, forever.
	[[nodiscard]] bool	TakeFailed( job_t & job );
	// draws the first job of the queue; false when the output is too small for any picture
	[[nodiscard]] bool	Draw();
	// After the frame Draw() was part of: the job and its pixels, RGB8, top row first.
	[[nodiscard]] bool	Finish( job_t & job, idList<byte> & rgb );

private:
	void				Init();
	void				Show( const job_t & job, renderView_t & view );

	idRenderWorld *		world = nullptr;
	idRenderModel *		sphere = nullptr;
	idRenderModel *		quad = nullptr;			// for the materials with a deform
	renderEntity_t		entity;					// set by Show()
	qhandle_t			entityHandle = -1;
	renderLight_t		lights[2];
	qhandle_t			lightHandles[2] = { -1, -1 };
	idJointMat *		joints = nullptr;
	idList<job_t>		queue;
	int					sincePurge = 0;			// pictures since the last purge of their images
	bool				drawing = false;		// between Draw() and Finish()
	int					drawnSize = 0;			// of the job in the frame being drawn, 0 = none
};
