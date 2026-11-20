// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "idlib/precompiled.h"

#include <windowsx.h>

#include "EditorView.h"

namespace {

constexpr float		FOV_X = 90.0f;
constexpr float		LOOK_DEGREES_PER_PIXEL = 0.15f;
constexpr float		MAX_PITCH = 89.0f;
constexpr float		WHEEL_STEP = 64.0f;				// units per wheel notch
constexpr float		FLY_SPEED_MIN = 25.0f;
constexpr float		FLY_SPEED_MAX = 6400.0f;
constexpr float		FLY_SPEED_STEP = 1.25f;			// per wheel notch
constexpr float		FAST_FACTOR = 3.0f;
constexpr int		MAX_FRAME_MSEC = 100;			// a hitch must not throw the camera

// one view per process: the window procedure finds it here
idEditorView *		attachedView = nullptr;

[[nodiscard]] bool KeyDown( int virtualKey ) {
	return ( GetAsyncKeyState( virtualKey ) & 0x8000 ) != 0;
}

}	// namespace

void idEditorView::Attach( HWND engineWindow ) {
	window = engineWindow;
	attachedView = this;
	engineProc = reinterpret_cast<WNDPROC>( SetWindowLongPtr( window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>( &WindowProc ) ) );
}

void idEditorView::SetEditMode( bool edit ) {
	if ( editMode == edit ) {
		return;
	}
	StopLook();
	// The engine may hold the focus from a game that just ended. It has to see the focus go:
	// in edit mode the message would be dropped and the engine would stay active.
	if ( edit && window != nullptr && GetFocus() == window ) {
		SetFocus( GetParent( window ) );
	}
	editMode = edit;
	// Outside a map nothing is drawn: without a clear the void keeps the frames before,
	// alternating between the swap chain's buffers. A game leaves it as it always was.
	cvarSystem->SetCVarString( "r_clear", edit ? "0.11 0.11 0.11" : "0" );
}

void idEditorView::SetView( const idVec3 & newOrigin, const idAngles & newAngles ) {
	origin = newOrigin;
	angles = newAngles;
	angles.roll = 0.0f;
}

void idEditorView::GetFov( float & fovX, float & fovY ) const {
	RECT rect = {};
	GetClientRect( window, &rect );
	const float aspect = static_cast<float>( Max( 1L, rect.right ) ) / static_cast<float>( Max( 1L, rect.bottom ) );
	fovX = FOV_X;
	fovY = RAD2DEG( 2.0f * idMath::ATan( idMath::Tan( DEG2RAD( fovX ) * 0.5f ) / aspect ) );
}

idVec3 idEditorView::RayThroughPixel( int x, int y ) const {
	RECT rect = {};
	GetClientRect( window, &rect );
	float fovX, fovY;
	GetFov( fovX, fovY );
	// -1 .. 1 across the window, left and up positive like the view axis
	const float left = 1.0f - 2.0f * ( static_cast<float>( x ) + 0.5f ) / static_cast<float>( Max( 1L, rect.right ) );
	const float up = 1.0f - 2.0f * ( static_cast<float>( y ) + 0.5f ) / static_cast<float>( Max( 1L, rect.bottom ) );
	const idMat3 axis = angles.ToMat3();
	idVec3 dir = axis[0] + axis[1] * ( left * idMath::Tan( DEG2RAD( fovX ) * 0.5f ) ) + axis[2] * ( up * idMath::Tan( DEG2RAD( fovY ) * 0.5f ) );
	dir.Normalize();
	return dir;
}

void idEditorView::StartLook() {
	if ( looking ) {
		return;
	}
	looking = true;
	GetCursorPos( &lookAnchor );
	SetCapture( window );
	// the keys go to this window and are dropped, not typed into a panel
	SetFocus( window );
	ShowCursor( FALSE );
}

void idEditorView::StopLook() {
	if ( !looking ) {
		return;
	}
	looking = false;
	ReleaseCapture();
	ShowCursor( TRUE );
	// the keys back to the host, for its shortcuts
	if ( GetFocus() == window ) {
		SetFocus( GetParent( window ) );
	}
}

void idEditorView::Frame() {
	const int now = Sys_Milliseconds();
	const float seconds = static_cast<float>( Min( now - lastFrameMsec, MAX_FRAME_MSEC ) ) * 0.001f;
	lastFrameMsec = now;
	if ( !editMode || !looking ) {
		return;
	}
	if ( GetCapture() != window ) {
		StopLook();
		return;
	}

	// the cursor is put back every frame, so its travel is the look delta
	POINT cursor;
	GetCursorPos( &cursor );
	if ( cursor.x != lookAnchor.x || cursor.y != lookAnchor.y ) {
		angles.yaw -= static_cast<float>( cursor.x - lookAnchor.x ) * LOOK_DEGREES_PER_PIXEL;
		angles.pitch = idMath::ClampFloat( -MAX_PITCH, MAX_PITCH, angles.pitch + static_cast<float>( cursor.y - lookAnchor.y ) * LOOK_DEGREES_PER_PIXEL );
		angles.yaw = idMath::AngleNormalize360( angles.yaw );
		SetCursorPos( lookAnchor.x, lookAnchor.y );
	}

	const idMat3 axis = angles.ToMat3();
	idVec3 move = vec3_origin;
	move += axis[0] * static_cast<float>( KeyDown( 'W' ) - KeyDown( 'S' ) );
	move -= axis[1] * static_cast<float>( KeyDown( 'D' ) - KeyDown( 'A' ) );
	move.z += static_cast<float>( KeyDown( 'E' ) - KeyDown( 'Q' ) );
	if ( move.Normalize() > 0.0f ) {
		const float speed = flySpeed * ( KeyDown( VK_SHIFT ) ? FAST_FACTOR : 1.0f );
		origin += move * ( speed * seconds );
	}
}

bool idEditorView::HandleMessage( UINT message, WPARAM wParam, LPARAM lParam, LRESULT & result ) {
	if ( !editMode ) {
		return false;
	}
	result = 0;
	switch ( message ) {
		case WM_MOUSEACTIVATE:
			result = MA_ACTIVATE;
			return true;
		case WM_RBUTTONDOWN:
			StartLook();
			return true;
		case WM_RBUTTONUP:
			StopLook();
			return true;
		case WM_CAPTURECHANGED:
			if ( reinterpret_cast<HWND>( lParam ) != window ) {
				StopLook();
				if ( leftDown ) {
					leftDown = false;
					client->ViewMouseUp();
				}
			}
			return true;
		case WM_LBUTTONDOWN:
			if ( !looking && client != nullptr ) {
				leftDown = true;
				SetCapture( window );
				client->ViewMouseDown( GET_X_LPARAM( lParam ), GET_Y_LPARAM( lParam ), ( wParam & MK_CONTROL ) != 0, ( wParam & MK_SHIFT ) != 0 );
			}
			return true;
		case WM_MOUSEMOVE:
			if ( leftDown ) {
				client->ViewMouseMove( GET_X_LPARAM( lParam ), GET_Y_LPARAM( lParam ) );
			}
			return true;
		case WM_LBUTTONUP:
			if ( leftDown ) {
				leftDown = false;
				ReleaseCapture();
				client->ViewMouseUp();
			}
			return true;
		case WM_MOUSEWHEEL: {
			const float notches = static_cast<float>( GET_WHEEL_DELTA_WPARAM( wParam ) ) / static_cast<float>( WHEEL_DELTA );
			if ( looking ) {
				flySpeed = idMath::ClampFloat( FLY_SPEED_MIN, FLY_SPEED_MAX, flySpeed * idMath::Pow( FLY_SPEED_STEP, notches ) );
			} else {
				origin += angles.ToForward() * ( notches * WHEEL_STEP );
			}
			return true;
		}
		case WM_SETCURSOR:
			SetCursor( LoadCursor( nullptr, IDC_ARROW ) );
			result = TRUE;
			return true;
		// what would make the engine active, or reach the game as input
		case WM_SETFOCUS:
		case WM_KILLFOCUS:
		case WM_KEYDOWN:
		case WM_KEYUP:
		case WM_SYSKEYDOWN:
		case WM_SYSKEYUP:
		case WM_CHAR:
		case WM_MBUTTONDOWN:
		case WM_MBUTTONUP:
		case WM_XBUTTONDOWN:
		case WM_XBUTTONUP:
			return true;
	}
	return false;
}

LRESULT CALLBACK idEditorView::WindowProc( HWND window, UINT message, WPARAM wParam, LPARAM lParam ) {
	idEditorView * const view = attachedView;
	LRESULT result = 0;
	if ( view->HandleMessage( message, wParam, lParam, result ) ) {
		return result;
	}
	return CallWindowProc( view->engineProc, window, message, wParam, lParam );
}
