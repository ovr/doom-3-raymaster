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
#ifndef __VISUALDEMO_H__
#define __VISUALDEMO_H__

/*
===============================================================================

	Visual demos: scripted scenes on the retail maps, run on a virtual clock and
	captured frame by frame for regression checks (the vdemo command, VisualDemo.cpp).

===============================================================================
*/

class idVisualDemo {
public:
	virtual					~idVisualDemo() = default;

	// from the vdemo command until its last demo ended
	[[nodiscard]] virtual bool	IsRunning() const = 0;

	// the frame rate cap while a demo runs: vdemo_fps with vdemo_realtime 1, else 0 (no cap)
	[[nodiscard]] virtual int	MaxFrameRate() const = 0;

	// The game clock of the frame loop's tic decision. ticMilliseconds is the length of the next
	// tic and residual the time already accumulated towards it. While a demo runs, each call is
	// one virtual step that ends exactly on a 1 / vdemo_fps grid of that tic.
	[[nodiscard]] virtual uint64	FrameLoopMicroseconds( int ticMilliseconds, double residual ) = 0;

	// The clock of the Flash GUIs: Sys_Milliseconds, but while a demo runs the virtual clock since
	// the demo began, so HUD animations, text effects and their random seeds repeat between runs.
	[[nodiscard]] virtual int	GuiMilliseconds() const = 0;

	// once per frame before the game thread starts: runs the timeline commands of this frame's
	// tics, replaces the player's usercmd and tags the frame the game thread is about to build
	virtual void			RunFrame( bool gameRunning, int numGameFrames, float renderAlpha, usercmd_t & cmd ) = 0;

	// the end of a map load
	virtual void			OnMapSpawned() = 0;

	// an ERP_DROP error: it unwinds the frame and leaves the map (or its load) for the menu
	virtual void			OnDropError() = 0;

	// right after the back end rendered a frame, before its swap
	virtual void			CaptureRenderedFrame() = 0;
};

extern idVisualDemo *		visualDemo;

#endif // !__VISUALDEMO_H__
