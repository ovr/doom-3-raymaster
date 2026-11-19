// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.
#pragma once

/*
	The edit view's camera and its input. While a map is being edited the engine's window is not
	the game's: its messages are taken before the engine's window procedure sees them, so the
	engine never becomes active (no mouse grab, no game keys).

	  hold the right button   look around, and fly with W A S D, Q / E down and up, Shift faster
	  left button             goes to the client: selection and the gizmo
	  wheel                   forward and back
	  wheel while flying      the flying speed

	Needs <windows.h> first.
*/
// what the left button does in the view; x and y are pixels of the engine window
class idEditorViewClient {
public:
	virtual void		ViewMouseDown( int x, int y, bool ctrl ) = 0;
	virtual void		ViewMouseMove( int x, int y ) = 0;
	virtual void		ViewMouseUp() = 0;

protected:
						~idEditorViewClient() = default;
};

class idEditorView {
public:
	void				SetClient( idEditorViewClient * newClient ) { client = newClient; }
	// takes over the messages of the engine window; edit mode starts off
	void				Attach( HWND engineWindow );
	void				SetEditMode( bool edit );
	[[nodiscard]] bool	EditMode() const { return editMode; }

	void				SetView( const idVec3 & newOrigin, const idAngles & newAngles );
	// moves the camera by what was held since the last call
	void				Frame();

	[[nodiscard]] const idVec3 &	Origin() const { return origin; }
	[[nodiscard]] idMat3	Axis() const { return angles.ToMat3(); }
	// the horizontal angle is fixed, the vertical one keeps the pixels square
	void				GetFov( float & fovX, float & fovY ) const;
	// the direction of the view ray through a pixel of the window
	[[nodiscard]] idVec3	RayThroughPixel( int x, int y ) const;

private:
	static LRESULT CALLBACK	WindowProc( HWND window, UINT message, WPARAM wParam, LPARAM lParam );
	// true when the message is the view's and the engine must not get it
	[[nodiscard]] bool	HandleMessage( UINT message, WPARAM wParam, LPARAM lParam, LRESULT & result );
	void				StartLook();
	void				StopLook();

	idEditorViewClient *	client = nullptr;
	bool				leftDown = false;
	HWND				window = nullptr;
	WNDPROC				engineProc = nullptr;
	bool				editMode = false;
	bool				looking = false;
	POINT				lookAnchor = {};

	idVec3				origin = vec3_origin;
	idAngles			angles = ang_zero;
	float				flySpeed = 300.0f;		// units per second
	int					lastFrameMsec = 0;
};
