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
#include "../../idlib/precompiled.h"

#include "win_local.h"
#include "rc/doom_resource.h"
#include "../../renderer/tr_local.h"

// last gamma ramp set by the renderer, re-applied after alt-tab
static unsigned short	s_gameGamma[3][256];
static bool				s_gameGammaValid = false;

// display mode set with ChangeDisplaySettingsEx, re-applied after alt-tab
static DEVMODE			s_cdsMode;
static char				s_cdsDevice[CCHDEVICENAME];

// last parms used to set up the window
static windowParms_t		s_lastParms;
static bool				s_lastParmsValid = false;

// true while the fullscreen window is deactivated (alt-tabbed away)
static bool				s_fullscreenSuspended = false;

/*
========================
Win_SaveGamma
========================
*/
static void Win_SaveGamma() {
	HDC			hDC;
	BOOL		success;

	hDC = GetDC( GetDesktopWindow() );
	success = GetDeviceGammaRamp( hDC, win32.oldHardwareGamma );
	common->DPrintf( "...getting default gamma ramp: %s\n", success ? "success" : "failed" );
	ReleaseDC( GetDesktopWindow(), hDC );
}

/*
========================
Win_RestoreGamma
========================
*/
static void Win_RestoreGamma() {
	HDC hDC;
	BOOL success;

	// if we never read in a reasonable looking
	// table, don't write it out
	if ( win32.oldHardwareGamma[0][255] == 0 ) {
		return;
	}

	hDC = GetDC( GetDesktopWindow() );
	success = SetDeviceGammaRamp( hDC, win32.oldHardwareGamma );
	common->DPrintf ( "...restoring hardware gamma: %s\n", success ? "success" : "failed" );
	ReleaseDC( GetDesktopWindow(), hDC );
}


/*
========================
Win_ApplyGameGamma

The ramp goes to the display the window is on.
========================
*/
static bool Win_ApplyGameGamma() {
	const HDC hDC = GetDC( win32.hWnd );
	if ( hDC == nullptr ) {
		return false;
	}
	const bool success = SetDeviceGammaRamp( hDC, s_gameGamma ) != FALSE;
	ReleaseDC( win32.hWnd, hDC );
	return success;
}

/*
========================
Sys_SetDeviceGammaRamp

The renderer calls this when the user adjusts r_gamma or r_brightness
========================
*/
void Sys_SetDeviceGammaRamp( const unsigned short red[256], const unsigned short green[256], const unsigned short blue[256] ) {
	if ( !win32.hWnd ) {
		return;
	}

	for ( int i = 0; i < 256; i++ ) {
		s_gameGamma[0][i] = red[i];
		s_gameGamma[1][i] = green[i];
		s_gameGamma[2][i] = blue[i];
	}
	s_gameGammaValid = true;

	// while alt-tabbed away the desktop gamma stays in effect,
	// the game ramp is applied again when the window is reactivated
	if ( s_fullscreenSuspended ) {
		return;
	}

	if ( !Win_ApplyGameGamma() ) {
		common->Printf( "WARNING: SetDeviceGammaRamp failed.\n" );
	}
}

/*
====================
Win_CreateWindowClass
====================
*/
static void Win_CreateWindowClass() {
	WNDCLASS wc;

	//
	// register the window class if necessary
	//
	if ( win32.windowClassRegistered ) {
		return;
	}

	memset( &wc, 0, sizeof( wc ) );

	wc.style         = 0;
	wc.lpfnWndProc   = (WNDPROC) MainWndProc;
	wc.cbClsExtra    = 0;
	wc.cbWndExtra    = 0;
	wc.hInstance     = win32.hInstance;
	wc.hIcon         = LoadIcon( win32.hInstance, MAKEINTRESOURCE(IDI_ICON1));
	wc.hCursor       = NULL;
	wc.hbrBackground = (struct HBRUSH__ *)COLOR_GRAYTEXT;
	wc.lpszMenuName  = 0;
	wc.lpszClassName = WIN32_WINDOW_CLASS_NAME;

	if ( !RegisterClass( &wc ) ) {
		common->FatalError( "GLW_CreateWindow: could not register window class" );
	}
	common->Printf( "...registered window class\n" );

	win32.windowClassRegistered = true;
}

/*
========================
GetDisplayName
========================
*/
static const char * GetDisplayName( const int deviceNum ) {
	static DISPLAY_DEVICE	device;
	device.cb = sizeof( device );
	if ( !EnumDisplayDevices(
			0,			// lpDevice
			deviceNum,
			&device,
			0 /* dwFlags */ ) ) {
		return NULL;
	}
	return device.DeviceName;
}

/*
========================
GetDeviceName
========================
*/
static idStr GetDeviceName( const int deviceNum ) {
	DISPLAY_DEVICE	device = {};
	device.cb = sizeof( device );
	if ( !EnumDisplayDevices(
			0,			// lpDevice
			deviceNum,
			&device,
			0 /* dwFlags */ ) ) {
		return idStr();
	}

	// get the monitor for this display
	if ( ! (device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP ) ) {
		return idStr();
	}

	return idStr( device.DeviceName );
}

/*
========================
GetDisplayCoordinates
========================
*/
static bool GetDisplayCoordinates( const int deviceNum, int & x, int & y, int & width, int & height, int & displayHz ) {
	idStr deviceName = GetDeviceName( deviceNum );
	if ( deviceName.Length() == 0 ) {
		return false;
	}

	DISPLAY_DEVICE	device = {};
	device.cb = sizeof( device );
	if ( !EnumDisplayDevices(
			0,			// lpDevice
			deviceNum,
			&device,
			0 /* dwFlags */ ) ) {
		return false;
	}

	DISPLAY_DEVICE	monitor;
	monitor.cb = sizeof( monitor );
	if ( !EnumDisplayDevices(
			deviceName.c_str(),
			0,
			&monitor,
			0 /* dwFlags */ ) ) {
		return false;
	}

	DEVMODE	devmode;
	devmode.dmSize = sizeof( devmode );
	if ( !EnumDisplaySettings( deviceName.c_str(),ENUM_CURRENT_SETTINGS, &devmode ) ) {
		return false;
	}

	common->Printf( "display device: %i\n", deviceNum );
	common->Printf( "  DeviceName  : %s\n", device.DeviceName );
	common->Printf( "  DeviceString: %s\n", device.DeviceString );
	common->Printf( "  StateFlags  : 0x%x\n", device.StateFlags );
	common->Printf( "  DeviceID    : %s\n", device.DeviceID );
	common->Printf( "  DeviceKey   : %s\n", device.DeviceKey );
	common->Printf( "      DeviceName  : %s\n", monitor.DeviceName );
	common->Printf( "      DeviceString: %s\n", monitor.DeviceString );
	common->Printf( "      StateFlags  : 0x%x\n", monitor.StateFlags );
	common->Printf( "      DeviceID    : %s\n", monitor.DeviceID );
	common->Printf( "      DeviceKey   : %s\n", monitor.DeviceKey );
	common->Printf( "          dmPosition.x      : %i\n", devmode.dmPosition.x );
	common->Printf( "          dmPosition.y      : %i\n", devmode.dmPosition.y );
	common->Printf( "          dmBitsPerPel      : %i\n", devmode.dmBitsPerPel );
	common->Printf( "          dmPelsWidth       : %i\n", devmode.dmPelsWidth );
	common->Printf( "          dmPelsHeight      : %i\n", devmode.dmPelsHeight );
	common->Printf( "          dmDisplayFlags    : 0x%x\n", devmode.dmDisplayFlags );
	common->Printf( "          dmDisplayFrequency: %i\n", devmode.dmDisplayFrequency );

	x = devmode.dmPosition.x;
	y = devmode.dmPosition.y;
	width = devmode.dmPelsWidth;
	height = devmode.dmPelsHeight;
	displayHz = devmode.dmDisplayFrequency;

	return true;
}

/*
====================
GetDesktopMode

The desktop resolution of a monitor as set in the Windows display settings,
not affected by a ChangeDisplaySettingsEx( CDS_FULLSCREEN ) we did.
====================
*/
static bool GetDesktopMode( const int deviceNum, int & width, int & height ) {
	idStr deviceName = GetDeviceName( deviceNum );
	if ( deviceName.Length() == 0 ) {
		return false;
	}

	DEVMODE	devmode = {};
	devmode.dmSize = sizeof( devmode );
	if ( !EnumDisplaySettings( deviceName.c_str(), ENUM_REGISTRY_SETTINGS, &devmode ) || devmode.dmPelsWidth == 0 ) {
		if ( !EnumDisplaySettings( deviceName.c_str(), ENUM_CURRENT_SETTINGS, &devmode ) ) {
			return false;
		}
	}

	width = devmode.dmPelsWidth;
	height = devmode.dmPelsHeight;
	return true;
}

/*
====================
R_GetDesktopResolution
====================
*/
bool R_GetDesktopResolution( const int displayNum, int & width, int & height ) {
	return GetDesktopMode( displayNum, width, height );
}

/*
====================
Win_ResolveBorderlessParms

A borderless fullscreen window always covers the monitor at its desktop resolution.
====================
*/
static void Win_ResolveBorderlessParms( windowParms_t & parms ) {
	if ( parms.fullScreen <= 0 ) {
		parms.borderless = false;
		return;
	}
	if ( !parms.borderless ) {
		return;
	}
	int width, height;
	if ( GetDesktopMode( parms.fullScreen - 1, width, height ) ) {
		parms.width = width;
		parms.height = height;
		parms.displayHz = 0;
	}
}

/*
====================
DMDFO
====================
*/
static const char * DMDFO( int dmDisplayFixedOutput ) {
	switch( dmDisplayFixedOutput ) {
	case DMDFO_DEFAULT: return "DMDFO_DEFAULT";
	case DMDFO_CENTER: return "DMDFO_CENTER";
	case DMDFO_STRETCH: return "DMDFO_STRETCH";
	}
	return "UNKNOWN";
}

/*
====================
PrintDevMode
====================
*/
static void PrintDevMode( DEVMODE & devmode ) {
	common->Printf( "          dmPosition.x        : %i\n", devmode.dmPosition.x );
	common->Printf( "          dmPosition.y        : %i\n", devmode.dmPosition.y );
	common->Printf( "          dmBitsPerPel        : %i\n", devmode.dmBitsPerPel );
	common->Printf( "          dmPelsWidth         : %i\n", devmode.dmPelsWidth );
	common->Printf( "          dmPelsHeight        : %i\n", devmode.dmPelsHeight );
	common->Printf( "          dmDisplayFixedOutput: %s\n", DMDFO( devmode.dmDisplayFixedOutput ) );
	common->Printf( "          dmDisplayFlags      : 0x%x\n", devmode.dmDisplayFlags );
	common->Printf( "          dmDisplayFrequency  : %i\n", devmode.dmDisplayFrequency );
}

/*
====================
DumpAllDisplayDevices
====================
*/
void DumpAllDisplayDevices() {
	common->Printf( "\n" );
	for ( int deviceNum = 0 ; ; deviceNum++ ) {
		DISPLAY_DEVICE	device = {};
		device.cb = sizeof( device );
		if ( !EnumDisplayDevices(
				0,			// lpDevice
				deviceNum,
				&device,
				0 /* dwFlags */ ) ) {
			break;
		}

		common->Printf( "display device: %i\n", deviceNum );
		common->Printf( "  DeviceName  : %s\n", device.DeviceName );
		common->Printf( "  DeviceString: %s\n", device.DeviceString );
		common->Printf( "  StateFlags  : 0x%x\n", device.StateFlags );
		common->Printf( "  DeviceID    : %s\n", device.DeviceID );
		common->Printf( "  DeviceKey   : %s\n", device.DeviceKey );

		for ( int monitorNum = 0 ; ; monitorNum++ ) {
			DISPLAY_DEVICE	monitor = {};
			monitor.cb = sizeof( monitor );
			if ( !EnumDisplayDevices(
					device.DeviceName,
					monitorNum,
					&monitor,
					0 /* dwFlags */ ) ) {
				break;
			}

			common->Printf( "      DeviceName  : %s\n", monitor.DeviceName );
			common->Printf( "      DeviceString: %s\n", monitor.DeviceString );
			common->Printf( "      StateFlags  : 0x%x\n", monitor.StateFlags );
			common->Printf( "      DeviceID    : %s\n", monitor.DeviceID );
			common->Printf( "      DeviceKey   : %s\n", monitor.DeviceKey );

			DEVMODE	currentDevmode = {};
			if ( !EnumDisplaySettings( device.DeviceName,ENUM_CURRENT_SETTINGS, &currentDevmode ) ) {
				common->Printf( "ERROR:  EnumDisplaySettings(ENUM_CURRENT_SETTINGS) failed!\n" );
			}
			common->Printf( "          -------------------\n" );
			common->Printf( "          ENUM_CURRENT_SETTINGS\n" );
			PrintDevMode( currentDevmode );

			DEVMODE	registryDevmode = {};
			if ( !EnumDisplaySettings( device.DeviceName,ENUM_REGISTRY_SETTINGS, &registryDevmode ) ) {
				common->Printf( "ERROR:  EnumDisplaySettings(ENUM_CURRENT_SETTINGS) failed!\n" );
			}
			common->Printf( "          -------------------\n" );
			common->Printf( "          ENUM_CURRENT_SETTINGS\n" );
			PrintDevMode( registryDevmode );

			for ( int modeNum = 0 ; ; modeNum++ ) {
				DEVMODE	devmode = {};

				if ( !EnumDisplaySettings( device.DeviceName,modeNum, &devmode ) ) {
					break;
				}

				if ( devmode.dmBitsPerPel != 32 ) {
					continue;
				}
				if ( devmode.dmDisplayFrequency < 60 ) {
					continue;
				}
				if ( devmode.dmPelsHeight < 720 ) {
					continue;
				}
				common->Printf( "          -------------------\n" );
				common->Printf( "          modeNum             : %i\n", modeNum );
				PrintDevMode( devmode );
			}
		}
	}
	common->Printf( "\n" );
}

/*
====================
R_GetModeListForDisplay
====================
*/
bool R_GetModeListForDisplay( const int requestedDisplayNum, idList<vidMode_t> & modeList ) {
	modeList.Clear();

	bool	verbose = false;

	for ( int displayNum = requestedDisplayNum; ; displayNum++ ) {
		DISPLAY_DEVICE	device;
		device.cb = sizeof( device );
		if ( !EnumDisplayDevices(
				0,			// lpDevice
				displayNum,
				&device,
				0 /* dwFlags */ ) ) {
			return false;
		}

		// get the monitor for this display
		if ( ! (device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP ) ) {
			continue;
		}

		DISPLAY_DEVICE	monitor;
		monitor.cb = sizeof( monitor );
		if ( !EnumDisplayDevices(
				device.DeviceName,
				0,
				&monitor,
				0 /* dwFlags */ ) ) {
			continue;
		}

		DEVMODE	devmode;
		devmode.dmSize = sizeof( devmode );

		if ( verbose ) {
			common->Printf( "display device: %i\n", displayNum );
			common->Printf( "  DeviceName  : %s\n", device.DeviceName );
			common->Printf( "  DeviceString: %s\n", device.DeviceString );
			common->Printf( "  StateFlags  : 0x%x\n", device.StateFlags );
			common->Printf( "  DeviceID    : %s\n", device.DeviceID );
			common->Printf( "  DeviceKey   : %s\n", device.DeviceKey );
			common->Printf( "      DeviceName  : %s\n", monitor.DeviceName );
			common->Printf( "      DeviceString: %s\n", monitor.DeviceString );
			common->Printf( "      StateFlags  : 0x%x\n", monitor.StateFlags );
			common->Printf( "      DeviceID    : %s\n", monitor.DeviceID );
			common->Printf( "      DeviceKey   : %s\n", monitor.DeviceKey );
		}

		for ( int modeNum = 0 ; ; modeNum++ ) {
			if ( !EnumDisplaySettings( device.DeviceName,modeNum, &devmode ) ) {
				break;
			}

			if ( devmode.dmBitsPerPel != 32 ) {
				continue;
			}
			if ( ( devmode.dmDisplayFrequency != 60 ) && ( devmode.dmDisplayFrequency != 120 ) ) {
				continue;
			}
			if ( devmode.dmPelsHeight < 720 ) {
				continue;
			}
			if ( verbose ) {
				common->Printf( "          -------------------\n" );
				common->Printf( "          modeNum             : %i\n", modeNum );
				common->Printf( "          dmPosition.x        : %i\n", devmode.dmPosition.x );
				common->Printf( "          dmPosition.y        : %i\n", devmode.dmPosition.y );
				common->Printf( "          dmBitsPerPel        : %i\n", devmode.dmBitsPerPel );
				common->Printf( "          dmPelsWidth         : %i\n", devmode.dmPelsWidth );
				common->Printf( "          dmPelsHeight        : %i\n", devmode.dmPelsHeight );
				common->Printf( "          dmDisplayFixedOutput: %s\n", DMDFO( devmode.dmDisplayFixedOutput ) );
				common->Printf( "          dmDisplayFlags      : 0x%x\n", devmode.dmDisplayFlags );
				common->Printf( "          dmDisplayFrequency  : %i\n", devmode.dmDisplayFrequency );
			}
			vidMode_t mode;
			mode.width = devmode.dmPelsWidth;
			mode.height = devmode.dmPelsHeight;
			mode.displayHz = devmode.dmDisplayFrequency;
			modeList.AddUnique( mode );
		}
		if ( modeList.Num() > 0 ) {

			class idSort_VidMode : public idSort_Quick< vidMode_t, idSort_VidMode > {
			public:
				int Compare( const vidMode_t & a, const vidMode_t & b ) const {
					int wd = a.width - b.width;
					int hd = a.height - b.height;
					int fd = a.displayHz - b.displayHz;
					return ( hd != 0 ) ? hd : ( wd != 0 ) ? wd : fd;
				}
			};

			// sort with lowest resolution first
			modeList.SortWithTemplate( idSort_VidMode() );

			return true;
		}
	}
	// Never gets here
}

/*
====================
Win_GetDisplayFrequency

The refresh rate in Hz of the monitor the window is on, 0 when unknown.
====================
*/
static int Win_GetDisplayFrequency() {
	MONITORINFOEX monitorInfo = {};
	monitorInfo.cbSize = sizeof( monitorInfo );
	if ( !GetMonitorInfo( MonitorFromWindow( win32.hWnd, MONITOR_DEFAULTTOPRIMARY ), &monitorInfo ) ) {
		return 0;
	}
	DEVMODE devmode = {};
	devmode.dmSize = sizeof( devmode );
	if ( !EnumDisplaySettings( monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &devmode ) ) {
		return 0;
	}
	// 0 and 1 mean the hardware default
	return devmode.dmDisplayFrequency > 1 ? static_cast<int>( devmode.dmDisplayFrequency ) : 0;
}

/*
====================
Win_GetWindowDimensions
====================
*/
static bool Win_GetWindowDimensions( const windowParms_t parms, int &x, int &y, int &w, int &h ) {
	//
	// compute width and height
	//
	if ( parms.fullScreen != 0 ) {
		if ( parms.fullScreen == -1 ) {
			// borderless window at specific location, as for spanning
			// multiple monitor outputs
			x = parms.x;
			y = parms.y;
			w = parms.width;
			h = parms.height;
		} else {
			// get the current monitor position and size on the desktop, assuming
			// any required ChangeDisplaySettings has already been done
			int displayHz = 0;
			if ( !GetDisplayCoordinates( parms.fullScreen - 1, x, y, w, h, displayHz ) ) {
				return false;
			}
		}
	} else {
		RECT	r;

		// adjust width and height for window border
		r.bottom = parms.height;
		r.left = 0;
		r.top = 0;
		r.right = parms.width;

		AdjustWindowRect (&r, WINDOW_STYLE|WS_SYSMENU, FALSE);

		w = r.right - r.left;
		h = r.bottom - r.top;

		x = parms.x;
		y = parms.y;
	}

	return true;
}


/*
=======================
Win_CreateWindow

Responsible for creating the Win32 window.
If fullscreen, it won't have a border
=======================
*/
static bool Win_CreateWindow( windowParms_t parms ) {
	int				x, y, w, h;
	if ( !Win_GetWindowDimensions( parms, x, y, w, h ) ) {
		return false;
	}

	int				stylebits;
	int				exstyle;
	if ( parms.fullScreen != 0 ) {
		// not WS_EX_TOPMOST, otherwise the window stays on top of
		// everything after alt-tab
		exstyle = WS_EX_APPWINDOW;
		stylebits = WS_POPUP|WS_VISIBLE|WS_SYSMENU;
	} else {
		exstyle = 0;
		stylebits = WINDOW_STYLE|WS_SYSMENU;
	}

	win32.hWnd = CreateWindowEx (
		 exstyle, 
		 WIN32_WINDOW_CLASS_NAME,
		 GAME_NAME,
		 stylebits,
		 x, y, w, h,
		 NULL,
		 NULL,
		 win32.hInstance,
		 NULL);

	if ( !win32.hWnd ) {
		common->Printf( "^3GLW_CreateWindow() - Couldn't create window^0\n" );
		return false;
	}

	::SetTimer( win32.hWnd, 0, 100, NULL );

	ShowWindow( win32.hWnd, SW_SHOW );
	UpdateWindow( win32.hWnd );
	common->Printf( "...created window @ %d,%d (%dx%d)\n", x, y, w, h );

	SetForegroundWindow( win32.hWnd );
	SetFocus( win32.hWnd );

	screenConfig.isFullscreen = parms.fullScreen;

	return true;
}

/*
===================
PrintCDSError
===================
*/
static void PrintCDSError( int value ) {
	switch ( value ) {
	case DISP_CHANGE_RESTART:
		common->Printf( "restart required\n" );
		break;
	case DISP_CHANGE_BADPARAM:
		common->Printf( "bad param\n" );
		break;
	case DISP_CHANGE_BADFLAGS:
		common->Printf( "bad flags\n" );
		break;
	case DISP_CHANGE_FAILED:
		common->Printf( "DISP_CHANGE_FAILED\n" );
		break;
	case DISP_CHANGE_BADMODE:
		common->Printf( "bad mode\n" );
		break;
	case DISP_CHANGE_NOTUPDATED:
		common->Printf( "not updated\n" );
		break;
	default:
		common->Printf( "unknown error %d\n", value );
		break;
	}
}

/*
===================
Win_ChangeDislaySettingsIfNeeded

Optionally ChangeDisplaySettings to get a different fullscreen resolution.
Default uses the full desktop resolution.
===================
*/
static bool Win_ChangeDislaySettingsIfNeeded( windowParms_t parms ) {
	// If we had previously changed the display settings on a different monitor,
	// go back to standard.
	if ( win32.cdsFullscreen != 0 && win32.cdsFullscreen != parms.fullScreen ) {
		win32.cdsFullscreen = 0;
		ChangeDisplaySettings( 0, 0 );
		Sys_Sleep( 1000 ); // Give the driver some time to think about this change
	}

	// 0 is dragable mode on desktop, -1 is borderless window on desktop
	if ( parms.fullScreen <= 0 ) {
		return true;
	}

	// borderless fullscreen never changes the display mode, go back to the
	// desktop mode if we changed it for exclusive fullscreen before
	if ( parms.borderless ) {
		if ( win32.cdsFullscreen != 0 ) {
			common->Printf( "...restoring desktop display mode\n" );
			ChangeDisplaySettingsEx( s_cdsDevice[0] ? s_cdsDevice : NULL, NULL, NULL, 0, NULL );
			win32.cdsFullscreen = 0;
		}
		return true;
	}

	// if we are already in the right resolution, don't do a ChangeDisplaySettings
	int x, y, width, height, displayHz;

	if ( !GetDisplayCoordinates( parms.fullScreen - 1, x, y, width, height, displayHz ) ) {
		return false;
	}
	if ( width == parms.width && height == parms.height && ( displayHz == parms.displayHz || parms.displayHz == 0 ) ) {
		return true;
	}

	DEVMODE dm = {};

	dm.dmSize = sizeof( dm );

	dm.dmPelsWidth  = parms.width;
	dm.dmPelsHeight = parms.height;
	dm.dmBitsPerPel = 32;
	dm.dmFields     = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
	if ( parms.displayHz != 0 ) {
		dm.dmDisplayFrequency = parms.displayHz;
		dm.dmFields |= DM_DISPLAYFREQUENCY;
	}
	
	common->Printf( "...calling CDS: " );
	
	const char * const deviceName = GetDisplayName( parms.fullScreen - 1 );

	int		cdsRet;
	if ( ( cdsRet = ChangeDisplaySettingsEx(
		deviceName,
		&dm, 
		NULL,
		CDS_FULLSCREEN,
		NULL) ) == DISP_CHANGE_SUCCESSFUL ) {
		common->Printf( "ok\n" );
		win32.cdsFullscreen = parms.fullScreen;
		s_cdsMode = dm;
		idStr::Copynz( s_cdsDevice, deviceName != NULL ? deviceName : "", sizeof( s_cdsDevice ) );
		return true;
	}

	common->Printf( "^3failed^0, " );
	PrintCDSError( cdsRet );
	return false;
}

/*
===================
Sys_CreateWindow

Registers the window class, sets the display mode and creates the
window. No graphics API is involved: the renderer backend attaches
to the window afterwards.

If there is any failure, the renderer will revert back to safe
parameters and try again.
===================
*/
bool Sys_CreateWindow( const windowParms_t & requestedParms ) {
	HDC		hDC;

	windowParms_t parms = requestedParms;
	Win_ResolveBorderlessParms( parms );

	common->Printf( "Creating the window with fullscreen:%i borderless:%i\n", parms.fullScreen, parms.borderless );

	// check our desktop attributes
	hDC = GetDC( GetDesktopWindow() );
	win32.desktopBitsPixel = GetDeviceCaps( hDC, BITSPIXEL );
	win32.desktopWidth = GetDeviceCaps( hDC, HORZRES );
	win32.desktopHeight = GetDeviceCaps( hDC, VERTRES );
	ReleaseDC( GetDesktopWindow(), hDC );

	// we can't run in a window unless it is 32 bpp
	if ( win32.desktopBitsPixel < 32 && parms.fullScreen <= 0 ) {
		common->Printf("^3Windowed mode requires 32 bit desktop depth^0\n");
		return false;
	}

	// save the hardware gamma so it can be
	// restored on exit
	Win_SaveGamma();

	// create our window class if we haven't already
	Win_CreateWindowClass();

	// Optionally ChangeDisplaySettings to get a different fullscreen resolution.
	if ( !Win_ChangeDislaySettingsIfNeeded( parms ) ) {
		Sys_DestroyWindow();
		return false;
	}

	if ( !Win_CreateWindow( parms ) ) {
		Sys_DestroyWindow();
		return false;
	}

	s_lastParms = parms;
	s_lastParmsValid = true;
	s_fullscreenSuspended = false;

	screenConfig.isFullscreen = parms.fullScreen;
	screenConfig.nativeScreenWidth = parms.width;
	screenConfig.nativeScreenHeight = parms.height;
	screenConfig.displayFrequency = Win_GetDisplayFrequency();

	screenConfig.pixelAspect = 1.0f;	// FIXME: some monitor modes may be distorted
									// should side-by-side stereo modes be consider aspect 0.5?

	// get the screen size, which may not be reliable...
	hDC = GetDC( win32.hWnd );
	const int mmWide = GetDeviceCaps( hDC, HORZSIZE );
	ReleaseDC( win32.hWnd, hDC );

	if ( mmWide == 0 ) {
		screenConfig.physicalScreenWidthInCentimeters = 100.0f;
	} else {
		screenConfig.physicalScreenWidthInCentimeters = 0.1f * mmWide;
	}

	return true;
}

/*
===================
Sys_SetWindowMode

Sets up the screen based on passed parms..
===================
*/
bool Sys_SetWindowMode( const windowParms_t & requestedParms ) {
	windowParms_t parms = requestedParms;
	Win_ResolveBorderlessParms( parms );

	// Optionally ChangeDisplaySettings to get a different fullscreen resolution.
	if ( !Win_ChangeDislaySettingsIfNeeded( parms ) ) {
		return false;
	}

	int x, y, w, h;
	if ( !Win_GetWindowDimensions( parms, x, y, w, h ) ) {
		return false;
	}

	int exstyle;
	int stylebits;

	if ( parms.fullScreen ) {
		exstyle = WS_EX_APPWINDOW;
		stylebits = WS_POPUP|WS_VISIBLE|WS_SYSMENU;
	} else {
		exstyle = 0;
		stylebits = WINDOW_STYLE|WS_SYSMENU;
	}

	SetWindowLong( win32.hWnd, GWL_STYLE, stylebits );
	SetWindowLong( win32.hWnd, GWL_EXSTYLE, exstyle );
	SetWindowPos( win32.hWnd, HWND_NOTOPMOST, x, y, w, h, SWP_SHOWWINDOW | SWP_FRAMECHANGED );

	s_lastParms = parms;
	s_lastParmsValid = true;
	s_fullscreenSuspended = false;

	screenConfig.isFullscreen = parms.fullScreen;
	screenConfig.pixelAspect = 1.0f;	// FIXME: some monitor modes may be distorted

	screenConfig.isFullscreen = parms.fullScreen;
	screenConfig.nativeScreenWidth = parms.width;
	screenConfig.nativeScreenHeight = parms.height;
	screenConfig.displayFrequency = Win_GetDisplayFrequency();

	return true;
}

/*
===================
Win_WindowDeactivated

Called from WM_ACTIVATE when the window loses focus (alt-tab, click on
another window, etc). A fullscreen window gives the desktop back: the
desktop gamma and display mode are restored and an exclusive fullscreen
window is minimized.
===================
*/
void Win_WindowDeactivated() {
	if ( win32.hWnd == NULL || !s_lastParmsValid || s_lastParms.fullScreen == 0 || s_fullscreenSuspended ) {
		return;
	}
	s_fullscreenSuspended = true;

	Win_RestoreGamma();

	if ( win32.cdsFullscreen ) {
		ChangeDisplaySettingsEx( s_cdsDevice[0] ? s_cdsDevice : NULL, NULL, NULL, 0, NULL );
	}

	// a borderless window just goes behind the other windows,
	// exclusive fullscreen gets out of the way
	if ( !s_lastParms.borderless && win32.win_minimizeOnFocusLoss.GetBool() ) {
		ShowWindow( win32.hWnd, SW_MINIMIZE );
	}
}

/*
===================
Win_WindowActivated

Called from WM_ACTIVATE when the window gets focus back.
Undoes Win_WindowDeactivated.
===================
*/
void Win_WindowActivated() {
	if ( win32.hWnd == NULL || !s_fullscreenSuspended ) {
		return;
	}
	s_fullscreenSuspended = false;

	if ( win32.cdsFullscreen ) {
		DEVMODE dm = s_cdsMode;
		if ( ChangeDisplaySettingsEx( s_cdsDevice[0] ? s_cdsDevice : NULL, &dm, NULL, CDS_FULLSCREEN, NULL ) != DISP_CHANGE_SUCCESSFUL ) {
			common->Printf( "^3Win_WindowActivated: failed to restore the display mode^0\n" );
		}
	}

	if ( IsIconic( win32.hWnd ) ) {
		ShowWindow( win32.hWnd, SW_RESTORE );
	}

	int x, y, w, h;
	if ( Win_GetWindowDimensions( s_lastParms, x, y, w, h ) ) {
		SetWindowPos( win32.hWnd, HWND_TOP, x, y, w, h, SWP_SHOWWINDOW );
	}

	if ( s_gameGammaValid ) {
		Win_ApplyGameGamma();
	}
}

/*
===================
Sys_DestroyWindow

Closes the window, resets the resolution and the gamma ramps.
The renderer backend must have released the window before.
===================
*/
void Sys_DestroyWindow() {
	// destroy window
	if ( win32.hWnd ) {
		common->Printf( "...destroying window\n" );
		ShowWindow( win32.hWnd, SW_HIDE );
		DestroyWindow( win32.hWnd );
		win32.hWnd = NULL;
	}

	// reset display settings
	if ( win32.cdsFullscreen ) {
		common->Printf( "...resetting display\n" );
		ChangeDisplaySettings( 0, 0 );
		win32.cdsFullscreen = 0;
	}

	// restore gamma
	Win_RestoreGamma();

	s_lastParmsValid = false;
	s_gameGammaValid = false;
	s_fullscreenSuspended = false;
}

/*
===================
Sys_GetNativeWindow
===================
*/
void * Sys_GetNativeWindow() {
	return win32.hWnd;
}
