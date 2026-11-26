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

#include "../idlib/precompiled.h"
#pragma hdrstop

#include "Common_local.h"
#include "EngineHost.h"
#include "VisualDemo.h"
#include "../renderer/Image.h"
#include "../renderer/ImageOpts.h"
#include "../../doomclassic/doom/doomlib.h"
#include "../../doomclassic/doom/globaldata.h"

/*

New for tech4x:

Unlike previous SMP work, the actual GPU command drawing is done in the main thread, which avoids the
OpenGL problems with needing windows to be created by the same thread that creates the context, as well
as the issues with passing context ownership back and forth on the 360.

The game tic and the generation of the draw command list is now run in a separate thread, and overlapped
with the interpretation of the previous draw command list.

While the game tic should be nicely contained, the draw command generation winds through the user interface
code, and is potentially hazardous.  For now, the overlap will be restricted to the renderer back end,
which should also be nicely contained.

*/
#define DEFAULT_FIXED_TIC "0"
#define DEFAULT_NO_SLEEP "0"

idCVar com_deltaTimeClamp( "com_deltaTimeClamp", "50", CVAR_INTEGER, "don't process more than this time in a single frame" );

idCVar com_fixedTic( "com_fixedTic", DEFAULT_FIXED_TIC, CVAR_INTEGER, "debug: run this many game tics per rendered frame, ties the game speed to the frame rate" );
idCVar com_noSleep( "com_noSleep", DEFAULT_NO_SLEEP, CVAR_BOOL, "debug: run a game tic every rendered frame instead of waiting for it, ties the game speed to the frame rate" );
idCVar com_smp( "com_smp", "1", CVAR_BOOL|CVAR_SYSTEM|CVAR_NOCHEAT, "run the game and draw code in a separate thread" );
idCVar com_decoupledTics( "com_decoupledTics", "1", CVAR_BOOL|CVAR_SYSTEM, "single player draws frames between game tics instead of waiting for the next tic; 0 = the old loop, which draws at most com_engineHz" );
idCVar com_maxFPS( "com_maxFPS", "0", CVAR_INTEGER|CVAR_SYSTEM|CVAR_ARCHIVE, "frame rate cap, also in menus and while paused; 0 = no cap", 0, 1000 );
idCVar com_aviDemoSamples( "com_aviDemoSamples", "16", CVAR_SYSTEM, "" );
idCVar com_aviDemoWidth( "com_aviDemoWidth", "256", CVAR_SYSTEM, "" );
idCVar com_aviDemoHeight( "com_aviDemoHeight", "256", CVAR_SYSTEM, "" );
idCVar com_skipGameDraw( "com_skipGameDraw", "0", CVAR_SYSTEM | CVAR_BOOL, "" );

idCVar com_sleepGame( "com_sleepGame", "0", CVAR_SYSTEM | CVAR_INTEGER, "intentionally add a sleep in the game time" );
idCVar com_sleepDraw( "com_sleepDraw", "0", CVAR_SYSTEM | CVAR_INTEGER, "intentionally add a sleep in the draw time" );
idCVar com_sleepRender( "com_sleepRender", "0", CVAR_SYSTEM | CVAR_INTEGER, "intentionally add a sleep in the render time" );

idCVar net_drawDebugHud( "net_drawDebugHud", "0", CVAR_SYSTEM | CVAR_INTEGER, "0 = None, 1 = Hud 1, 2 = Hud 2, 3 = Snapshots" );

idCVar timescale( "timescale", "1", CVAR_SYSTEM | CVAR_FLOAT, "Number of game frames to run per render frame", 0.001f, 100.0f );

extern idCVar in_useJoystick;
extern idCVar in_joystickRumble;

/*
===============
idGameThread::Run

Run in a background thread for performance, but can also
be called directly in the foreground thread for comparison.
===============
*/
int idGameThread::Run() {
	// an ERP_DROP error must not unwind into idSysThread, which exits the process without a word
	try {
		RunFrame();
	} catch ( idException & ex ) {
		error = ex.GetError();
	}
	return 0;
}

/*
===============
idGameThread::RethrowError
===============
*/
void idGameThread::RethrowError() {
	if ( error.IsEmpty() ) {
		return;
	}
	const idStr message = error;
	error.Clear();
	throw idException( message.c_str() );
}

/*
===============
idGameThread::RunFrame
===============
*/
void idGameThread::RunFrame() {
	// let the sampling profiler know which thread runs the game (cheap after the first call)
	Sys_ProfilerRegisterThread();

	commonLocal.frameTiming.startGameTime = Sys_Microseconds();

	// debugging tool to test frame dropping behavior
	if ( com_sleepGame.GetInteger() ) {
		Sys_Sleep( com_sleepGame.GetInteger() );
	}

	if ( frame.paused ) {
		// Ensure there's no stale gameReturn data from a paused game
		ret = gameReturn_t();
	} else {
		// the session already has the last one-shot results (a stale syncNextGameFrame ran a tic
		// every frame); the rumble stays, a frame between two tics doesn't set it again
		ret.syncNextGameFrame = false;
		ret.sessionCommand[0] = '\0';
	}

	const int numGameFrames = frame.numGameFrames;
	if ( frame.isClient ) {
		// run the game logic
		for ( int i = 0; i < numGameFrames; i++ ) {
			SCOPED_PROFILE_EVENT( "Client Prediction" );
			if ( userCmdMgr ) {
				game->ClientRunFrame( *userCmdMgr, ( i == numGameFrames - 1 ), ret );
			}
			if ( ret.syncNextGameFrame || ret.sessionCommand[0] != 0 ) {
				break;
			}
		}
	} else {
		// run the game logic
		for ( int i = 0; i < numGameFrames; i++ ) {
			SCOPED_PROFILE_EVENT( "GameTic" );
			if ( userCmdMgr ) {
				game->RunFrame( *userCmdMgr, ret );
			}
			if ( ret.syncNextGameFrame || ret.sessionCommand[0] != 0 ) {
				break;
			}
		}
	}

	// we should have consumed all of our usercmds
	if ( userCmdMgr ) {
		if ( userCmdMgr->HasUserCmdForPlayer( game->GetLocalClientNum() ) && common->GetCurrentGame() == DOOM3_BFG ) {
			idLib::Printf( "idGameThread::Run: didn't consume all usercmds\n" );
		}
	}

	commonLocal.frameTiming.finishGameTime = Sys_Microseconds();

	SetThreadGameTime( ( commonLocal.frameTiming.finishGameTime - commonLocal.frameTiming.startGameTime ) / 1000 );

	// build render commands and geometry
	{
		SCOPED_PROFILE_EVENT( "Draw" );
		if ( game != nullptr ) {
			game->SetRenderFrame( frame.renderAlpha, frame.latestCmd );
		}
		commonLocal.Draw();
	}

	commonLocal.frameTiming.finishDrawTime = Sys_Microseconds();

	SetThreadRenderTime( ( commonLocal.frameTiming.finishDrawTime - commonLocal.frameTiming.finishGameTime ) / 1000 );

	SetThreadTotalTime( ( commonLocal.frameTiming.finishDrawTime - commonLocal.frameTiming.startGameTime ) / 1000 );
}

/*
===============
idGameThread::RunGameAndDraw

===============
*/
gameReturn_t idGameThread::RunGameAndDraw( const gameThreadFrame_t & frame_, idUserCmdMgr & userCmdMgr_ ) {
	// this should always immediately return
	this->WaitForThread();

	// save the usercmds for the background thread to pick up
	userCmdMgr = &userCmdMgr_;

	// grab the return value created by the last thread execution
	gameReturn_t latchedRet = ret;

	frame = frame_;

	// start the thread going
	if ( com_smp.GetBool() == false ) {
		// run it in the main thread so PIX profiling catches everything
		Run();
		RethrowError();
	} else {
		this->SignalWork();
	}

	// return the latched result while the thread runs in the background
	return latchedRet;
}


/*
===============
idCommonLocal::DrawWipeModel

Draw the fade material over everything that has been drawn
===============
*/
void idCommonLocal::DrawWipeModel() {

	if ( wipeStartTime >= wipeStopTime ) {
		return;
	}

	int currentTime = Sys_Milliseconds();

	if ( !wipeHold && currentTime > wipeStopTime ) {
		return;
	}

	float fade = ( float )( currentTime - wipeStartTime ) / ( wipeStopTime - wipeStartTime );
	renderSystem->SetColor4( 1, 1, 1, fade );
	renderSystem->DrawStretchPic( 0, 0, 640, 480, 0, 0, 1, 1, wipeMaterial );
}

/*
===============
idCommonLocal::Draw
===============
*/
void idCommonLocal::Draw() {
	// debugging tool to test frame dropping behavior
	if ( com_sleepDraw.GetInteger() ) {
		Sys_Sleep( com_sleepDraw.GetInteger() );
	}

	if ( loadGUI != NULL ) {
		loadGUI->Render( renderSystem, Sys_Milliseconds() );
	} else if ( engineHost != nullptr && !mapSpawned && engineHost->DrawScene() ) {
		// the host drew its own world
	} else if ( currentGame == DOOM_CLASSIC || currentGame == DOOM2_CLASSIC ) {
		const float sysWidth = renderSystem->GetWidth() * renderSystem->GetPixelAspect();
		const float sysHeight = renderSystem->GetHeight();
		const float sysAspect = sysWidth / sysHeight;
		const float doomAspect = 4.0f / 3.0f;
		const float adjustment = sysAspect / doomAspect;
		const float barHeight = ( adjustment >= 1.0f ) ? 0.0f : ( 1.0f - adjustment ) * (float)SCREEN_HEIGHT * 0.25f;
		const float barWidth = ( adjustment <= 1.0f ) ? 0.0f : ( adjustment - 1.0f ) * (float)SCREEN_WIDTH * 0.25f;
		if ( barHeight > 0.0f ) {
			renderSystem->SetColor( colorBlack );
			renderSystem->DrawStretchPic( 0, 0, SCREEN_WIDTH, barHeight, 0, 0, 1, 1, whiteMaterial );
			renderSystem->DrawStretchPic( 0, SCREEN_HEIGHT - barHeight, SCREEN_WIDTH, barHeight, 0, 0, 1, 1, whiteMaterial );
		}
		if ( barWidth > 0.0f ) {
			renderSystem->SetColor( colorBlack );
			renderSystem->DrawStretchPic( 0, 0, barWidth, SCREEN_HEIGHT, 0, 0, 1, 1, whiteMaterial );
			renderSystem->DrawStretchPic( SCREEN_WIDTH - barWidth, 0, barWidth, SCREEN_HEIGHT, 0, 0, 1, 1, whiteMaterial );
		}
		renderSystem->SetColor4( 1, 1, 1, 1 );
		renderSystem->DrawStretchPic( barWidth, barHeight, SCREEN_WIDTH - barWidth * 2.0f, SCREEN_HEIGHT - barHeight * 2.0f, 0, 0, 1, 1, doomClassicMaterial );
	} else if ( game && game->Shell_IsActive() ) {
		bool gameDraw = game->Draw( game->GetLocalClientNum() );
		if ( !gameDraw ) {
			renderSystem->SetColor( colorBlack );
			renderSystem->DrawStretchPic( 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0, 0, 1, 1, whiteMaterial );
		}
		game->Shell_Render();
	} else if ( readDemo ) {
		renderWorld->RenderScene( &currentDemoRenderView );
		renderSystem->DrawDemoPics();
	} else if ( mapSpawned ) {
		bool gameDraw = false;
		// normal drawing for both single and multi player
		if ( !com_skipGameDraw.GetBool() && Game()->GetLocalClientNum() >= 0 ) {
			// draw the game view
			int	start = Sys_Milliseconds();
			if ( game ) {
				gameDraw = game->Draw( Game()->GetLocalClientNum() );
			}
			int end = Sys_Milliseconds();
			time_gameDraw += ( end - start );	// note time used for com_speeds
		}
		if ( !gameDraw ) {
			renderSystem->SetColor( colorBlack );
			renderSystem->DrawStretchPic( 0, 0, 640, 480, 0, 0, 1, 1, whiteMaterial );
		}

		// save off the 2D drawing from the game
		if ( writeDemo ) {
			renderSystem->WriteDemoPics();
		}
	} else {
		renderSystem->SetColor4( 0, 0, 0, 1 );
		renderSystem->DrawStretchPic( 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0, 0, 1, 1, whiteMaterial );
	}

	{
		SCOPED_PROFILE_EVENT( "Post-Draw" );

		// draw the wipe material on top of this if it hasn't completed yet
		DrawWipeModel();

		Dialog().Render( loadGUI != NULL );

		// draw the half console / notify console on top of everything
		console->Draw( false );
	}
}

/*
===============
idCommonLocal::UpdateScreen

This is an out-of-sequence screen update, not the normal game rendering
===============
*/
void idCommonLocal::UpdateScreen( bool captureToImage ) {
	if ( insideUpdateScreen ) {
		return;
	}
	insideUpdateScreen = true;

	// make sure the game / draw thread has completed
	gameThread.WaitForThread();

	// release the mouse capture back to the desktop
	Sys_GrabMouseCursor( false );

	// build all the draw commands without running a new game tic
	Draw();

	if ( captureToImage ) {
		renderSystem->CaptureRenderToImage( "_currentRender", false );
	}

	// this should exit right after vsync, with the GPU idle and ready to draw
	const emptyCommand_t * cmd = renderSystem->SwapCommandBuffers( &time_frontend, &time_backend, &time_shadows, &time_gpu );

	// get the GPU busy with new commands
	renderSystem->RenderCommandBuffers( cmd );

	insideUpdateScreen = false;
}
/*
================
idCommonLocal::ProcessGameReturn
================
*/
void idCommonLocal::ProcessGameReturn( const gameReturn_t & ret ) {
	// set joystick rumble
	if ( in_useJoystick.GetBool() && in_joystickRumble.GetBool() && !game->Shell_IsActive() && session->GetSignInManager().GetMasterInputDevice() >= 0 ) {
		Sys_SetRumble( session->GetSignInManager().GetMasterInputDevice(), ret.vibrationLow, ret.vibrationHigh );		// Only set the rumble on the active controller
	} else {
		for ( int i = 0; i < MAX_INPUT_DEVICES; i++ ) {
			Sys_SetRumble( i, 0, 0 );
		}
	}

	syncNextGameFrame = ret.syncNextGameFrame;

	if ( ret.sessionCommand[0] ) {
		idCmdArgs args;

		args.TokenizeString( ret.sessionCommand, false );

		if ( !idStr::Icmp( args.Argv(0), "map" ) ) {
			MoveToNewMap( args.Argv( 1 ), false );
		} else if ( !idStr::Icmp( args.Argv(0), "devmap" ) ) {
			MoveToNewMap( args.Argv( 1 ), true );
		} else if ( !idStr::Icmp( args.Argv(0), "died" ) ) {
			if ( !IsMultiplayer() ) {
				game->Shell_Show( true );
			}
		} else if ( !idStr::Icmp( args.Argv(0), "disconnect" ) ) {
			cmdSystem->BufferCommandText( CMD_EXEC_INSERT, "stoprecording ; disconnect" );
		} else if ( !idStr::Icmp( args.Argv(0), "endOfDemo" ) ) {
			cmdSystem->BufferCommandText( CMD_EXEC_NOW, "endOfDemo" );
		}
	}
}

/*
=================
idCommonLocal::DecoupledTics
=================
*/
bool idCommonLocal::DecoupledTics() {
	// multiplayer sends one usercmd per tic, Doom Classic runs one tic per Frame and
	// an AVI capture wants one tic per captured frame
	return com_decoupledTics.GetBool() && !IsMultiplayer() && !IsPlayingDoomClassic() && !aviCaptureMode;
}

/*
=================
idCommonLocal::GetTargetFrameRate
=================
*/
float idCommonLocal::GetTargetFrameRate() {
	const float engineHz = Max( 1.0f, com_engineHz_latched );
	float hz = static_cast<float>( com_maxFPS.GetInteger() );
	if ( hz <= 0.0f ) {
		hz = static_cast<float>( renderSystem->GetDisplayRefreshRate() );
	}
	if ( hz <= 0.0f ) {
		hz = engineHz;
	}
	return DecoupledTics() ? hz : Min( hz, engineHz );
}

/*
=================
idCommonLocal::LimitFrameRate

Waits for the next frame start at maxFPS, 0 = no wait. It paces real time, not the game clock
of the tic decision, so it reads Sys_Microseconds itself.
=================
*/
void idCommonLocal::LimitFrameRate( const int maxFPS ) {
	if ( maxFPS <= 0 ) {
		limiterFrameUS = 0;
		return;
	}
	const uint64 periodUS = ( 1000000 + maxFPS / 2 ) / maxFPS;
	const uint64 targetUS = limiterFrameUS + periodUS;
	const uint64 nowUS = Sys_Microseconds();
	if ( limiterFrameUS != 0 && nowUS < targetUS ) {
		Sys_WaitUntilMicroseconds( targetUS );
		limiterFrameUS = targetUS;
	} else if ( limiterFrameUS != 0 && nowUS < targetUS + periodUS ) {
		// less than a frame late: keep the cadence, so the average stays at maxFPS
		limiterFrameUS = targetUS;
	} else {
		limiterFrameUS = nowUS;
	}
}

extern idCVar com_forceGenericSIMD;

/*
=================
idCommonLocal::Frame
=================
*/
void idCommonLocal::Frame() {
	try {
		SCOPED_PROFILE_EVENT( "Common::Frame" );

		// This is the only place this is incremented
		idLib::frameNumber++;

		// allow changing SIMD usage on the fly
		if ( com_forceGenericSIMD.IsModified() ) {
			idSIMD::InitProcessor( "doom", com_forceGenericSIMD.GetBool() );
			com_forceGenericSIMD.ClearModified();
		}

		// Do the actual switch between Doom 3 and the classics here so
		// that things don't get confused in the middle of the frame.
		PerformGameSwitch();

		// pump all the events
		Sys_GenerateEvents();

		// write config file if anything changed
		WriteConfiguration(); 

		eventLoop->RunEventLoop();

		// Activate the shell if it's been requested
		if ( showShellRequested && game ) {
			game->Shell_Show( true );
			showShellRequested = false;
		}

		// if the console or another gui is down, we don't need to hold the mouse cursor
		bool chatting = false;
		// a host without a running map has no menu either: the mouse is the host's
		const bool hostIdle = ( engineHost != nullptr && !mapSpawned );
		if ( hostIdle || console->Active() || Dialog().IsDialogActive() || session->IsSystemUIShowing() || ( game && game->InhibitControls() && !IsPlayingDoomClassic() ) ) {
			Sys_GrabMouseCursor( false );
			usercmdGen->InhibitUsercmd( INHIBIT_SESSION, true );
			chatting = true;
		} else {
			Sys_GrabMouseCursor( true );
			usercmdGen->InhibitUsercmd( INHIBIT_SESSION, false );
		}

		const bool pauseGame = ( !mapSpawned || ( !IsMultiplayer() && ( Dialog().IsDialogPausing() || session->IsSystemUIShowing() || ( game && game->Shell_IsActive() ) ) ) ) && !IsPlayingDoomClassic();

		// save the screenshot of the last game frame if needed (no audio: AVIOpen and AVIClose are stubs)
		if ( aviCaptureMode ) {
			const idStr name = va( "demos/%s/%s_%05i.tga", aviDemoShortName.c_str(), aviDemoShortName.c_str(), aviDemoFrameCount++ );

			// remove any printed lines at the top before taking the screenshot
			console->ClearNotifyLines();

			// this calls Draw com_aviDemoSamples times, with r_jitter when it is > 1
			renderSystem->TakeScreenshot( com_aviDemoWidth.GetInteger(), com_aviDemoHeight.GetInteger(), name, com_aviDemoSamples.GetInteger(), nullptr );
		}

		//--------------------------------------------
		// wait for the GPU to finish drawing
		//
		// It is imporant to minimize the time spent between this
		// section and the call to renderSystem->RenderCommandBuffers(),
		// because the GPU is completely idle.
		//--------------------------------------------
		// this should exit right after vsync, with the GPU idle and ready to draw
		// This may block if the GPU isn't finished renderng the previous frame.
		frameTiming.startSyncTime = Sys_Microseconds();
		const emptyCommand_t * renderCommands = NULL;
		if ( com_smp.GetBool() ) {
			renderCommands = renderSystem->SwapCommandBuffers( &time_frontend, &time_backend, &time_shadows, &time_gpu );
		} else {
			// the GPU will stay idle through command generation for minimal
			// input latency
			renderSystem->SwapCommandBuffers_FinishRendering( &time_frontend, &time_backend, &time_shadows, &time_gpu );
		}
		frameTiming.finishSyncTime = Sys_Microseconds();

		//--------------------------------------------
		// Determine how many game tics we are going to run, now that the previous frame is
		// completely finished. Waiting on the GPU and for com_maxFPS must come before this,
		// or dropping frames for performance management stutters badly.
		//
		//   residual += min( time since the last decision, com_deltaTimeClamp ) * timescale
		//   paused                 no tic; coupled: residual = 0; decoupled: residual and alpha stay
		//   com_fixedTic N, sync   N or 1 tics, residual = 0, alpha = 1
		//   otherwise              run every tic that is due ( while residual >= tic length )
		//     none due, noSleep    1 tic, residual = 0, alpha = 1
		//     none due, decoupled  draw anyway, alpha = residual / tic length
		//     none due, coupled    sleep and decide again; the coupled loop always draws alpha 1
		//--------------------------------------------
		// a visual demo's virtual clock steps once a frame: uncapped it runs as fast as it can,
		// with vdemo_realtime 1 it waits for each step's real time
		LimitFrameRate( visualDemo->IsRunning() ? visualDemo->MaxFrameRate() : com_maxFPS.GetInteger() );

		const bool decoupled = DecoupledTics();

		// the game clock of the tic decision, the only place it reads the time; a running visual
		// demo replaces it with a virtual clock that steps 1 / vdemo_fps on the grid of the next tic
		const auto frameTimeUS = [this] {
			const int ticMilliseconds = FRAME_TO_MSEC( gameFrame + 1 ) - FRAME_TO_MSEC( gameFrame );
			return visualDemo->FrameLoopMicroseconds( ticMilliseconds, gameTimeResidual );
		};

		int numGameFrames = 0;
		bool showLastTic = !decoupled;

		for(;;) {
			const uint64 thisFrameTimeUS = frameTimeUS();
			if ( lastFrameTimeUS == 0 ) {
				lastFrameTimeUS = thisFrameTimeUS;
			}
			const double deltaMilliseconds = static_cast<double>( thisFrameTimeUS - lastFrameTimeUS ) / 1000.0;
			lastFrameTimeUS = thisFrameTimeUS;

			// if there was a large gap in time since the last frame, or the frame
			// rate is very very low, limit the number of frames we will run
			const double clampedDeltaMilliseconds = Min( deltaMilliseconds, static_cast<double>( com_deltaTimeClamp.GetInteger() ) );

			// don't run any frames when paused
			if ( pauseGame ) {
				gameFrame++;
				if ( !decoupled ) {
					gameTimeResidual = 0;
				}
				break;
			}

			gameTimeResidual += clampedDeltaMilliseconds * timescale.GetFloat();

			// debug cvar to force multiple game tics
			if ( com_fixedTic.GetInteger() > 0 ) {
				numGameFrames = com_fixedTic.GetInteger();
				gameFrame += numGameFrames;
				gameTimeResidual = 0;
				showLastTic = true;
				break;
			}

			if ( syncNextGameFrame ) {
				// don't sleep at all
				syncNextGameFrame = false;
				gameFrame++;
				numGameFrames++;
				gameTimeResidual = 0;
				showLastTic = true;
				break;
			}

			for ( ;; ) {
				// How much time to wait before running the next frame,
				// based on com_engineHz
				const int frameDelay = FRAME_TO_MSEC( gameFrame + 1 ) - FRAME_TO_MSEC( gameFrame );
				if ( gameTimeResidual < frameDelay ) {
					break;
				}
				gameTimeResidual -= frameDelay;
				gameFrame++;
				numGameFrames++;
				// if there is enough residual left, we may run additional frames
			}

			if ( numGameFrames > 0 ) {
				// ready to actually run them
				break;
			}

			// if we are vsyncing, we always want to run at least one game
			// frame and never sleep, which might happen due to scheduling issues
			// if we were just looking at real time.
			if ( com_noSleep.GetBool() ) {
				numGameFrames = 1;
				gameFrame += numGameFrames;
				gameTimeResidual = 0;
				showLastTic = true;
				break;
			}

			if ( decoupled ) {
				break;
			}

			// not enough time has passed to run a frame, as might happen if
			// we don't have vsync on, or the monitor is running at 120hz while
			// com_engineHz is 60, so sleep a bit and check again
			Sys_Sleep( 0 );
		}

		if ( !pauseGame ) {
			const int ticMilliseconds = FRAME_TO_MSEC( gameFrame + 1 ) - FRAME_TO_MSEC( gameFrame );
			const float alpha = ticMilliseconds > 0 ? static_cast<float>( gameTimeResidual / ticMilliseconds ) : 1.0f;
			renderAlpha = showLastTic ? 1.0f : idMath::ClampFloat( 0.0f, 1.0f, alpha );
		}
		const uint64 finishTicWaitTime = Sys_Microseconds();

		//--------------------------------------------
		// It would be better to push as much of this as possible
		// either before or after the renderSystem->SwapCommandBuffers(),
		// because the GPU is completely idle.
		//--------------------------------------------

		// Update session and syncronize to the new session state after sleeping
		session->UpdateSignInManager();
		session->Pump();
		session->ProcessSnapAckQueue();

		if ( session->GetState() == idSession::LOADING ) {
			// If the session reports we should be loading a map, load it!
			ExecuteMapChange();
			mapSpawnData.savegameFile = NULL;
			mapSpawnData.persistentPlayerInfo.Clear();
			return;
		} else if ( session->GetState() != idSession::INGAME && mapSpawned ) {
			// If the game is running, but the session reports we are not in a game, disconnect
			// This happens when a server disconnects us or we sign out
			LeaveGame();
			return;
		}

		if ( mapSpawned && !pauseGame ) {
			if ( IsClient() ) {
				RunNetworkSnapshotFrame();
			}
		}

		ExecuteReliableMessages();

		// send frame and mouse events to active guis
		GuiFrameEvents();

		//--------------------------------------------
		// Prepare usercmds and kick off the game processing
		// in a background thread
		//--------------------------------------------

		// build a new usercmd, every frame; the coupled loop treats each build as a tic's
		const bool ticFrame = !decoupled || numGameFrames > 0;
		const int deviceNum = session->GetSignInManager().GetMasterInputDevice();
		usercmdGen->BuildCurrentUsercmd( deviceNum, ticFrame, decoupled );
		if ( deviceNum == -1 ) {
			for ( int i = 0; i < MAX_INPUT_DEVICES; i++ ) {
				Sys_PollJoystickInputEvents( i );
				Sys_EndJoystickInputEvents();
			}
		}
		if ( pauseGame ) {
			usercmdGen->Clear();
		}

		usercmd_t newCmd = usercmdGen->GetCurrentUsercmd();

		// a running visual demo replaces the input and runs its timeline for the tics of this frame
		visualDemo->RunFrame( mapSpawned && !pauseGame, numGameFrames, renderAlpha, newCmd );

		// Store server game time - don't let time go past last SS time in case we are extrapolating
		if ( IsClient() ) {
			newCmd.serverGameMilliseconds = std::min( Game()->GetServerGameTimeMs(), Game()->GetSSEndTime() );
		} else {
			newCmd.serverGameMilliseconds = Game()->GetServerGameTimeMs();
		}

		userCmdMgr.MakeReadPtrCurrentForPlayer( Game()->GetLocalClientNum() );

		// Stuff a copy of this userCmd for each game frame we are going to run.
		// Ideally, the usercmds would be built in another thread so you could
		// still get 60hz control accuracy when the game is running slower.
		for ( int i = 0 ; i < numGameFrames ; i++ ) {
			newCmd.clientGameMilliseconds = FRAME_TO_MSEC( gameFrame-numGameFrames+i+1 );
			userCmdMgr.PutUserCmdForPlayer( game->GetLocalClientNum(), newCmd );
		}

		// If we're in Doom or Doom 2, run tics and upload the new texture.
		if ( ( GetCurrentGame() == DOOM_CLASSIC || GetCurrentGame() == DOOM2_CLASSIC ) && !( Dialog().IsDialogPausing() || session->IsSystemUIShowing() ) ) {
			RunDoomClassicFrame();
		}
		
		// start the game / draw command generation thread going in the background
		gameThreadFrame_t threadFrame;
		threadFrame.numGameFrames = numGameFrames;
		threadFrame.paused = pauseGame;
		threadFrame.isClient = IsClient();
		threadFrame.renderAlpha = renderAlpha;
		threadFrame.latestCmd = newCmd;
		gameReturn_t ret = gameThread.RunGameAndDraw( threadFrame, userCmdMgr );

		if ( !com_smp.GetBool() ) {
			// in non-smp mode, run the commands we just generated, instead of
			// frame-delayed ones from a background thread
			renderCommands = renderSystem->SwapCommandBuffers_FinishCommandBuffers();
		}

		//----------------------------------------
		// Run the render back end, getting the GPU busy with new commands
		// ASAP to minimize the pipeline bubble.
		//----------------------------------------
		frameTiming.startRenderTime = Sys_Microseconds();
		renderSystem->RenderCommandBuffers( renderCommands );
		// the back buffer holds the frame until the next swap
		visualDemo->CaptureRenderedFrame();
		if ( com_sleepRender.GetInteger() > 0 ) {
			// debug tool to test frame adaption
			Sys_Sleep( com_sleepRender.GetInteger() );
		}
		frameTiming.finishRenderTime = Sys_Microseconds();

		// make sure the game / draw thread has completed
		// This may block if the game is taking longer than the render back end
		gameThread.WaitForThread();
		const uint64 finishGameWaitTime = Sys_Microseconds();

		// print what the game thread printed this frame
		FlushThreadedPrints();
		gameThread.RethrowError();

		// Send local usermds to the server.
		// This happens after the game frame has run so that prediction data is up to date.
		SendUsercmds( Game()->GetLocalClientNum() );

		// Now that we have an updated game frame, we can send out new snapshots to our clients
		session->Pump(); // Pump to get updated usercmds to relay
		SendSnapshots();

		// Render the sound system using the latest commands from the game thread
		if ( pauseGame ) {
			soundWorld->Pause();
			soundSystem->SetPlayingSoundWorld( menuSoundWorld );
		} else {
			soundWorld->UnPause();
			soundSystem->SetPlayingSoundWorld( soundWorld );
		}
		soundSystem->Render();

		// process the game return for map changes, etc
		ProcessGameReturn( ret );

		idLobbyBase & lobby = session->GetActivePlatformLobbyBase();
		if ( lobby.HasActivePeers() ) {
			if ( net_drawDebugHud.GetInteger() == 1 ) {
				lobby.DrawDebugNetworkHUD();
			}
			if ( net_drawDebugHud.GetInteger() == 2 ) {
				lobby.DrawDebugNetworkHUD2();
			}
			lobby.DrawDebugNetworkHUD_ServerSnapshotMetrics( net_drawDebugHud.GetInteger() == 3 );
		}

		// report timing information
		// All times are in milliseconds with 0.01 precision:
		//   all  - wall time since the previous report
		//   sync - waiting for the previous frame's swap / GPU fence
		//   wait - com_maxFPS limiter, and in the coupled loop the wait for the next game tic
		//   pre  - session, gui events and usercmd generation
		//   game - game logic on the game thread (all tics of this frame)
		//   draw - renderer front end on the game thread
		//   bk   - renderer back end (command list recording) on the main thread
		//   join - main thread waiting for the game thread after the back end
		//   post - sound, snapshots and the game return processing
		//   gpu  - GPU time of the previous frame (timer query)
		//   tics - game tics run this frame, alpha - the frame's position between two tics
		if ( com_speeds.GetBool() ) {
			static uint64 lastTime = Sys_Microseconds();
			const uint64 nowTime = Sys_Microseconds();
			const uint64 frameMicroSec = nowTime - lastTime;
			lastTime = nowTime;
			const double toMs = 1.0 / 1000.0;
			Printf( "frame:%d all:%.2f sync:%.2f wait:%.2f pre:%.2f game:%.2f draw:%.2f bk:%.2f join:%.2f post:%.2f gpu:%.2f tics:%d alpha:%.2f\n",
				idLib::frameNumber,
				frameMicroSec * toMs,
				( frameTiming.finishSyncTime - frameTiming.startSyncTime ) * toMs,
				( finishTicWaitTime - frameTiming.finishSyncTime ) * toMs,
				( frameTiming.startRenderTime - finishTicWaitTime ) * toMs,
				( frameTiming.finishGameTime - frameTiming.startGameTime ) * toMs,
				( frameTiming.finishDrawTime - frameTiming.finishGameTime ) * toMs,
				( frameTiming.finishRenderTime - frameTiming.startRenderTime ) * toMs,
				( finishGameWaitTime - frameTiming.finishRenderTime ) * toMs,
				( nowTime - finishGameWaitTime ) * toMs,
				time_gpu * toMs,
				numGameFrames,
				renderAlpha );
			time_gameFrame = 0;
			time_gameDraw = 0;
		}

		mainFrameTiming = frameTiming;

		session->GetSaveGameManager().Pump();
	} catch( idException & ) {
		return;			// an ERP_DROP was thrown
	}
}

/*
=================
idCommonLocal::RunDoomClassicFrame
=================
*/
void idCommonLocal::RunDoomClassicFrame() {
	static int doomTics = 0;

	if( DoomLib::expansionDirty ) {

		// re-Initialize the Doom Engine.
		DoomLib::Interface.Shutdown();
		DoomLib::Interface.Startup( 1, false );
		DoomLib::expansionDirty = false;
	}


	if ( DoomLib::Interface.Frame( doomTics, &userCmdMgr ) ) {
		Globals *data = (Globals*)DoomLib::GetGlobalData( 0 );

		idArray< unsigned int, 256 > palette;
		std::copy( data->XColorMap, data->XColorMap + palette.Num(), palette.Ptr() );

		// Do the palette lookup.
		for ( int row = 0; row < DOOMCLASSIC_RENDERHEIGHT; ++row ) {
			for ( int column = 0; column < DOOMCLASSIC_RENDERWIDTH; ++column ) {
				const int doomScreenPixelIndex = row * DOOMCLASSIC_RENDERWIDTH + column;
				const byte paletteIndex = data->screens[0][doomScreenPixelIndex];
				const unsigned int paletteColor = palette[paletteIndex];
				const byte red = (paletteColor & 0xFF000000) >> 24;
				const byte green = (paletteColor & 0x00FF0000) >> 16;
				const byte blue = (paletteColor & 0x0000FF00) >> 8;

				const int imageDataPixelIndex = row * DOOMCLASSIC_RENDERWIDTH * DOOMCLASSIC_BYTES_PER_PIXEL + column * DOOMCLASSIC_BYTES_PER_PIXEL;
				doomClassicImageData[imageDataPixelIndex]		= red;
				doomClassicImageData[imageDataPixelIndex + 1]	= green;
				doomClassicImageData[imageDataPixelIndex + 2]	= blue;
				doomClassicImageData[imageDataPixelIndex + 3]	= 255;
			}
		}
	}

	renderSystem->UploadImage( "_doomClassic", doomClassicImageData.Ptr(), DOOMCLASSIC_RENDERWIDTH, DOOMCLASSIC_RENDERHEIGHT );
	doomTics++;
}
