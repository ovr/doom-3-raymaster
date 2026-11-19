// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "idlib/precompiled.h"

#include <string>
#include <string_view>

#include "framework/EngineHost.h"
#include "sys/sys_window.h"
#include "sys/win32/win_entry.h"

/*
	Doom3Editor.exe: the engine hosted in the editor's own window.

	--bare (for now the only mode) is a plain resizable frame around the engine viewport, with the
	game's own input. It tests the hosting itself, on both render APIs, without the Qt interface:

	  Doom3Editor.exe --bare +devmap game/mc_underground
*/

namespace {

constexpr char		BARE_CLASS_NAME[] = "Doom3EditorBare";
constexpr char		BARE_TITLE[] = "Doom 3 Editor (bare)";
constexpr int		BARE_WIDTH = 1280;
constexpr int		BARE_HEIGHT = 720;

class idBareHost final : public idEngineHost {
public:
	[[nodiscard]] bool	Create( HINSTANCE instance );
	// the engine window exists from here on: hand it the activation the frame already got
	void				EngineStarted();
	[[nodiscard]] bool	QuitRequested() const { return quit; }

	void *				ParentWindow() override { return frame; }
	void				GetViewportSize( int & width, int & height ) override;
	void				PumpEvents() override;
	void				OnQuitRequested() override { quit = true; }
	void				OnFatalError( const char * text ) override;

private:
	static LRESULT CALLBACK	WndProc( HWND window, UINT message, WPARAM wParam, LPARAM lParam );
	[[nodiscard]] HWND	Viewport() const { return engineStarted ? static_cast<HWND>( Sys_GetNativeWindow() ) : nullptr; }

	HWND				frame = nullptr;
	bool				engineStarted = false;
	bool				quit = false;
};

idBareHost *		bareHost = nullptr;

bool idBareHost::Create( HINSTANCE instance ) {
	WNDCLASSA windowClass = {};
	windowClass.lpfnWndProc = WndProc;
	windowClass.hInstance = instance;
	windowClass.hCursor = LoadCursor( nullptr, IDC_ARROW );
	windowClass.lpszClassName = BARE_CLASS_NAME;
	if ( RegisterClassA( &windowClass ) == 0 ) {
		return false;
	}

	const DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
	RECT rect = { 0, 0, BARE_WIDTH, BARE_HEIGHT };
	AdjustWindowRect( &rect, style, FALSE );

	bareHost = this;
	frame = CreateWindowExA( 0, BARE_CLASS_NAME, BARE_TITLE, style, CW_USEDEFAULT, CW_USEDEFAULT,
		rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr );
	if ( frame == nullptr ) {
		return false;
	}
	ShowWindow( frame, SW_SHOW );
	return true;
}

void idBareHost::EngineStarted() {
	engineStarted = true;
	const bool active = ( GetForegroundWindow() == frame );
	Sys_SetAppActive( active );
	if ( active ) {
		SetFocus( Viewport() );
	}
}

void idBareHost::GetViewportSize( int & width, int & height ) {
	RECT rect = {};
	GetClientRect( frame, &rect );
	width = Max( 1, static_cast<int>( rect.right ) );
	height = Max( 1, static_cast<int>( rect.bottom ) );
}

void idBareHost::PumpEvents() {
	MSG message;
	while ( PeekMessageA( &message, nullptr, 0, 0, PM_REMOVE ) ) {
		if ( message.message == WM_QUIT ) {
			quit = true;
			continue;
		}
		TranslateMessage( &message );
		DispatchMessageA( &message );
	}
}

void idBareHost::OnFatalError( const char * text ) {
	MessageBoxA( frame, text, BARE_TITLE, MB_OK | MB_ICONERROR );
}

LRESULT CALLBACK idBareHost::WndProc( HWND window, UINT message, WPARAM wParam, LPARAM lParam ) {
	idBareHost * const host = bareHost;
	const HWND viewport = ( host != nullptr ) ? host->Viewport() : nullptr;

	switch ( message ) {
		case WM_SIZE:
			if ( viewport != nullptr && wParam != SIZE_MINIMIZED ) {
				SetWindowPos( viewport, nullptr, 0, 0, LOWORD( lParam ), HIWORD( lParam ), SWP_NOZORDER | SWP_NOACTIVATE );
			}
			return 0;
		case WM_ACTIVATE:
			// the engine child never gets this message
			if ( viewport != nullptr ) {
				const bool active = ( LOWORD( wParam ) != WA_INACTIVE );
				Sys_SetAppActive( active );
				if ( active ) {
					SetFocus( viewport );
				}
			}
			return 0;
		case WM_SETFOCUS:
			if ( viewport != nullptr ) {
				SetFocus( viewport );
			}
			return 0;
		case WM_ERASEBKGND:
			// the viewport covers the whole client area
			return 1;
		case WM_CLOSE:
			if ( host != nullptr ) {
				host->quit = true;
			}
			return 0;
	}
	return DefWindowProcA( window, message, wParam, lParam );
}

// the editor's own switches start with "--" and are not for the engine's command line
[[nodiscard]] bool TakeSwitch( std::string & cmdLine, std::string_view name ) {
	const size_t at = cmdLine.find( name );
	if ( at == std::string::npos ) {
		return false;
	}
	cmdLine.erase( at, name.size() );
	return true;
}

}	// namespace

int WINAPI WinMain( _In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ LPSTR cmdLine, _In_ int ) {
	std::string engineCmdLine = cmdLine;
	// the Qt interface comes later: every start is the bare shell so far
	(void)TakeSwitch( engineCmdLine, "--bare" );

	// One thread: nothing of a frame is pending between two frames, so the editor can change the world there.
	// A window inside the host's, whatever the saved video mode says.
	engineCmdLine += " +set com_smp 0 +set r_fullscreen 0";

	idBareHost host;
	if ( !host.Create( instance ) ) {
		MessageBoxA( nullptr, "Could not create the editor window.", BARE_TITLE, MB_OK | MB_ICONERROR );
		return 1;
	}

	Sys_EngineInit( instance, engineCmdLine.c_str(), &host );
	host.EngineStarted();

	while ( !host.QuitRequested() ) {
		Sys_EngineFrame();
	}

	Sys_EngineShutdown();
	return 0;
}
