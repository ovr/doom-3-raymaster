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
#include "../idlib/precompiled.h"

#include "tr_local.h"
#include "../framework/Common_local.h"

idCVar r_drawFlickerBox( "r_drawFlickerBox", "0", CVAR_RENDERER | CVAR_BOOL, "visual test for dropping frames" );
idCVar stereoRender_warp( "stereoRender_warp", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "use the optical warping renderprog instead of stereoDeGhost" );
idCVar stereoRender_warpStrength( "stereoRender_warpStrength", "1.45", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "amount of pre-distortion" );

idCVar stereoRender_warpCenterX( "stereoRender_warpCenterX", "0.5", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "center for left eye, right eye will be 1.0 - this" );
idCVar stereoRender_warpCenterY( "stereoRender_warpCenterY", "0.5", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "center for both eyes" );
idCVar stereoRender_warpParmZ( "stereoRender_warpParmZ", "0", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "development parm" );
idCVar stereoRender_warpParmW( "stereoRender_warpParmW", "0", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "development parm" );
idCVar stereoRender_warpTargetFraction( "stereoRender_warpTargetFraction", "1.0", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "fraction of half-width the through-lens view covers" );

void RB_SetMVP( const idRenderMatrix & mvp );

static void RB_ViewportAndScissor( int x, int y, int w, int h ) {
	renderBackend->Viewport( x, y, w, h );
	renderBackend->Scissor( x, y, w, h );
}

/*
============================================================================

RENDER BACK END THREAD FUNCTIONS

============================================================================
*/

/*
=============
RB_DrawFlickerBox
=============
*/
static void RB_DrawFlickerBox() {
	if ( !r_drawFlickerBox.GetBool() ) {
		return;
	}
	renderBackend->Scissor( 0, 0, 256, 256 );
	if ( tr.frameCount & 1 ) {
		renderBackend->Clear( true, false, false, 0, 1.0f, 0.0f, 0.0f, 1.0f );
	} else {
		renderBackend->Clear( true, false, false, 0, 0.0f, 1.0f, 0.0f, 1.0f );
	}
}

// r_testClearPattern: tools/vdemo/check-pattern.py rebuilds the frame from these rectangles, in GL
// coordinates (origin at the bottom left); keep the two in step.
static void RB_DrawTestClearPattern() {
	if ( !r_testClearPattern.GetBool() ) {
		return;
	}
	const auto clearRect = []( int x, int y, int width, int height, int r, int g, int b ) {
		renderBackend->Scissor( x, y, width, height );
		renderBackend->Clear( true, false, false, 0, r / 255.0f, g / 255.0f, b / 255.0f, 1.0f );
	};
	const int width = tr.GetWidth();
	const int height = tr.GetHeight();
	renderBackend->State( 0 );
	renderBackend->Scissor( 0, 0, width, height );
	renderBackend->Clear( true, true, true, 0, 32 / 255.0f, 64 / 255.0f, 96 / 255.0f, 1.0f );
	clearRect( 0, 0, 64, 32, 255, 0, 0 );
	clearRect( width - 64, 0, 64, 32, 0, 255, 0 );
	clearRect( 0, height - 32, 64, 32, 0, 0, 255 );
	clearRect( width - 64, height - 32, 64, 32, 255, 255, 255 );
	clearRect( 100, 50, 1, 1, 255, 255, 0 );
	renderBackend->State( GLS_GREENMASK | GLS_BLUEMASK | GLS_ALPHAMASK );
	clearRect( width / 2 - 64, height / 2 - 32, 128, 64, 255, 255, 255 );
}

// R13: the 3D view the HDR scene resolves at: the last drawing one of a world that the front end did not mark
// (viewDef_t::notMainView), before the first drawing GUI view. Subviews, the portal sky and remote views before it
// draw into the scene too; a second view of its world, the PDA, menus and previews stay LDR.
// In stereo only that eye's views count, and GUI views among the other eye's are that eye's full-screen FX.
static const drawSurfsCommand_t * RB_FindHdrMainView( const emptyCommand_t * cmds, int stereoEye ) {
	if ( !renderBackend->Caps().hdrScene ) {
		return nullptr;
	}
	const drawSurfsCommand_t * found = nullptr;
	bool otherEye = false;
	for ( ; cmds != nullptr; cmds = (const emptyCommand_t *)cmds->next ) {
		if ( cmds->commandId != RC_DRAW_VIEW_3D && cmds->commandId != RC_DRAW_VIEW_GUI ) {
			continue;
		}
		const drawSurfsCommand_t * dsc = (const drawSurfsCommand_t *)cmds;
		const viewDef_t * viewDef = dsc->viewDef;
		const int viewEye = viewDef->renderView.viewEyeBuffer;
		if ( stereoEye != 0 && viewEye != 0 ) {
			otherEye = ( viewEye != stereoEye );
			if ( otherEye ) {
				continue;
			}
		}
		if ( !RB_ViewDraws( viewDef ) ) {
			continue;
		}
		if ( cmds->commandId == RC_DRAW_VIEW_GUI ) {
			if ( otherEye ) {
				continue;
			}
			break;
		}
		if ( !viewDef->isSubview && !viewDef->notMainView && viewDef->viewEntitys != nullptr ) {
			found = dsc;
		}
	}
	if ( found != nullptr && !found->viewDef->postProcess.hdr ) {
		found = nullptr;
	}
	if ( r_hdrDebug.GetInteger() == 2 ) {
		common->Printf( "hdr: %s\n", ( found == nullptr ) ? "LDR frame" : va( "main view %d x %d, eye %d", found->viewDef->viewport.GetWidth(),
			found->viewDef->viewport.GetHeight(), stereoEye ) );
	}
	return found;
}

// r_hdrDebug 1: what the eye adaptation of the last read-back resolve chose, once a second; 3: every frame, exactly
// and with the view's time, so two runs can be compared line by line.
static void RB_PrintHdrExposure( const viewDef_t & viewDef ) {
	static int lastPrint;
	const int now = Sys_Milliseconds();
	const std::optional<hdrExposure_t> exposure = renderBackend->LastHdrExposure();
	if ( !exposure ) {
		return;
	}
	if ( r_hdrDebug.GetInteger() == 3 ) {
		common->Printf( "hdr %d: EV100 measured %.6f, target %.6f, adapted %.6f; exposure %.6g, preExposure %.6g\n", viewDef.renderView.time[0],
			exposure->measuredEV100, exposure->targetEV100, exposure->adaptedEV100, exposure->exposure, exposure->preExposure );
		return;
	}
	if ( now - lastPrint < 1000 ) {
		return;
	}
	lastPrint = now;
	common->Printf( "hdr: EV100 measured %.2f, target %.2f, adapted %.2f; exposure %.3f (%+.2f EV), preExposure %.3f\n", exposure->measuredEV100,
		exposure->targetEV100, exposure->adaptedEV100, exposure->exposure, idMath::Log( Max( exposure->exposure, 1e-30f ) ) / idMath::Log( 2.0f ),
		exposure->preExposure );
}

static void RB_BeginHdrScene( const drawSurfsCommand_t * mainView ) {
	if ( mainView != nullptr && ( r_hdrDebug.GetInteger() == 1 || r_hdrDebug.GetInteger() == 3 ) ) {
		RB_PrintHdrExposure( *mainView->viewDef );
	}
	if ( mainView != nullptr ) {
		renderBackend->BeginHdrScene( mainView->viewDef->postProcess );
		backEnd.hdrPhase = true;
		backEnd.hdrGamma = false;
	}
}

// A phase no resolve ended: the 2D that follows must not draw into the HDR scene.
static void RB_EndHdrScene() {
	if ( backEnd.hdrPhase ) {
		renderBackend->EndHdrScene();
		backEnd.hdrPhase = false;
		backEnd.hdrGamma = false;
	}
}

// the debug tools that draw last, then the end of the frame (mono and stereo)
static void RB_EndFrame() {
	backEnd.tracedScene = backEnd.shadowLatch.EndFrame();
	RB_EndHdrScene();
	RB_DrawFlickerBox();
	RB_DrawTestClearPattern();
	renderBackend->EndDrawStream();

	// the timestamps and debug groups of r_pix close inside the frame; EndFrame hands the frame to the GPU
	renderLog.EndFrame();
	renderBackend->EndFrame();
}

// The colour clear at the start of a frame, or of a stereo eye, which is always cleared (black by default).
// r_clear 0 leaves a mono frame what the frame before left in the scene target, as the mono path always
// did. r_singleArea leaves pixels unrendered, so it clears too.
static void RB_SetBuffer( bool stereoEye ) {
	const bool requested = r_clear.GetFloat() != 0.0f || idStr::Length( r_clear.GetString() ) != 1 || r_singleArea.GetBool();
	if ( !requested && !stereoEye ) {
		return;
	}
	RENDERLOG_PRINTF( "---------- RB_SetBuffer ----------\n" );

	renderBackend->Scissor( 0, 0, tr.GetWidth(), tr.GetHeight() );
	idVec3 c( 0.0f, 0.0f, 0.0f );
	if ( requested && sscanf( r_clear.GetString(), "%f %f %f", &c.x, &c.y, &c.z ) != 3 ) {
		// a partial parse has written c.x ("2" gives red): r_clear 2 is black, anything else the debug purple
		c = r_clear.GetInteger() == 2 ? idVec3( 0.0f, 0.0f, 0.0f ) : idVec3( 0.4f, 0.0f, 0.25f );
	}
	renderBackend->Clear( true, false, false, 0, c.x, c.y, c.z, 1.0f );
}

/*
====================
R_MakeStereoRenderImage
====================
*/
static void R_MakeStereoRenderImage( idImage *image ) {
	idImageOpts	opts;
	opts.width = renderSystem->GetWidth();
	opts.height = renderSystem->GetHeight();
	opts.numLevels = 1;
	opts.format = FMT_RGBA8;
	image->SetRole( textureRole_t::COLOR_COPY );
	image->AllocImage( opts, TF_LINEAR, TR_CLAMP );
}

/*
====================
RB_SetStereoImageSampler

A copy from the output leaves the texture linear and clamped to its edge whatever the image
remembers, so the sampler is sent again every frame.
====================
*/
static void RB_SetStereoImageSampler( idImage * image, textureFilter_t filter, textureRepeat_t repeat ) {
	image->SetSamplerState( filter, repeat );
	image->UpdateSampler();
}

/*
====================
RB_StereoRenderExecuteBackEndCommands

Renders the draw list twice, with slight modifications for left eye / right eye
====================
*/
void RB_StereoRenderExecuteBackEndCommands( const emptyCommand_t * const allCmds ) {
	uint64 backEndStartTime = Sys_Microseconds();

	// create the stereoRenderImage if we haven't already
	static idImage * stereoRenderImages[2];
	for ( int i = 0; i < 2; i++ ) {
		if ( stereoRenderImages[i] == NULL ) {
			stereoRenderImages[i] = globalImages->ImageFromFunction( va("_stereoRender%i",i), R_MakeStereoRenderImage );
		}

		// resize the stereo render image if the main window has changed size
		if ( stereoRenderImages[i]->GetUploadWidth() != renderSystem->GetWidth() ||
			 stereoRenderImages[i]->GetUploadHeight() != renderSystem->GetHeight() ) {
			stereoRenderImages[i]->Resize( renderSystem->GetWidth(), renderSystem->GetHeight() );
		}
	}

	// In stereoRender mode, the front end has generated two RC_DRAW_VIEW commands
	// with slightly different origins for each eye.

	// TODO: only do the copy after the final view has been rendered, not mirror subviews?

	// Render the 3D draw views from the screen origin so all the screen relative
	// texture mapping works properly, then copy the portion we are going to use
	// off to a texture.
	bool foundEye[2] = { false, false };

	for ( int stereoEye = 1; stereoEye >= -1; stereoEye -= 2 ) {
		// set up the target texture we will draw to
		const int targetEye = ( stereoEye == 1 ) ? 1 : 0;
		
		// Set the back end into a known default state to fix any stale render state issues
		renderBackend->SetDefaultState();
		renderProgManager.Unbind();
		renderProgManager.ZeroUniforms();
		const drawSurfsCommand_t * const hdrView = RB_FindHdrMainView( allCmds, stereoEye );
		RB_BeginHdrScene( hdrView );

		for ( const emptyCommand_t * cmds = allCmds; cmds != NULL; cmds = (const emptyCommand_t *)cmds->next ) {
			switch ( cmds->commandId ) {
			case RC_NOP:
				break;
			case RC_DRAW_VIEW_GUI:
			case RC_DRAW_VIEW_3D:
				{
					const drawSurfsCommand_t * const dsc = (const drawSurfsCommand_t *)cmds;
					const viewDef_t		&	eyeViewDef = *dsc->viewDef;

					if ( eyeViewDef.renderView.viewEyeBuffer && eyeViewDef.renderView.viewEyeBuffer != stereoEye ) {
						// this is the render view for the other eye
						continue;
					}

					foundEye[ targetEye ] = true;
					RB_DrawView( dsc, stereoEye, dsc == hdrView );
					if ( dsc == hdrView ) {
						RB_EndHdrScene();
					}
				}
				break;
			case RC_SET_BUFFER:
				RB_SetBuffer( true );
				break;
			case RC_COPY_RENDER:
				RB_CopyRender( cmds );
				break;
			case RC_POST_PROCESS:
				{
					postProcessCommand_t * cmd = (postProcessCommand_t *)cmds;
					if ( cmd->viewDef->renderView.viewEyeBuffer != stereoEye ) {
						break;
					}
					RB_PostProcess( cmds );
				}
				break;
			default:
				common->Error( "RB_ExecuteBackEndCommands: bad commandId" );
				break;
			}
		}

		// copy to the target
		RB_EndHdrScene();
		stereoRenderImages[ targetEye ]->CopyFramebuffer( 0, 0, renderSystem->GetWidth(), renderSystem->GetHeight() );
	}

	// perform the final compositing / warping / deghosting to the actual framebuffer(s)
	assert( foundEye[0] && foundEye[1] );

	renderBackend->SetDefaultState();

	RB_SetMVP( renderMatrix_identity );

	renderBackend->State( GLS_DEPTHFUNC_ALWAYS );
	renderBackend->Cull( CT_TWO_SIDED );

	// We just want to do a quad pass - so make sure we disable any texgen and
	// set the texture matrix to the identity so we don't get anomalies from 
	// any stale uniform data being present from a previous draw call
	const float texS[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
	const float texT[4] = { 0.0f, 1.0f, 0.0f, 0.0f };
	renderProgManager.SetRenderParm( RENDERPARM_TEXTUREMATRIX_S, texS );
	renderProgManager.SetRenderParm( RENDERPARM_TEXTUREMATRIX_T, texT );

	// disable any texgen
	const float texGenEnabled[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	renderProgManager.SetRenderParm( RENDERPARM_TEXGEN_0_ENABLED, texGenEnabled );

	renderProgManager.BindShader_Texture();
	RB_SetColor( 1, 1, 1, 1 );

	switch( renderSystem->GetStereo3DMode() ) {
	case STEREO3D_HDMI_720:
		// HDMI 720P 3D
		stereoRenderImages[1]->Bind( 0 );
		stereoRenderImages[0]->Bind( 1 );
		RB_ViewportAndScissor( 0, 0, 1280, 720 );
		RB_DrawElementsWithCounters( &backEnd.unitSquareSurface );

		stereoRenderImages[0]->Bind( 0 );
		stereoRenderImages[1]->Bind( 1 );
		RB_ViewportAndScissor( 0, 750, 1280, 720 );
		RB_DrawElementsWithCounters( &backEnd.unitSquareSurface );

		// force the HDMI 720P 3D guard band to a constant color
		renderBackend->Scissor( 0, 720, 1280, 30 );
		renderBackend->Clear( true, false, false, 0, 0.0f, 0.0f, 0.0f, 1.0f );
		break;
	default:
	case STEREO3D_SIDE_BY_SIDE:
		if ( stereoRender_warp.GetBool() ) {
			// this is the Rift warp
			// renderSystem->GetWidth() / GetHeight() have returned equal values (640 for initial Rift)
			// and we are going to warp them onto a symetric square region of each half of the screen

			renderProgManager.BindShader_StereoWarp();

			// clear the entire screen to black
			// we could be smart and only clear the areas we aren't going to draw on, but
			// clears are fast...
			renderBackend->Scissor( 0, 0, screenConfig.nativeScreenWidth, screenConfig.nativeScreenHeight );
			renderBackend->Clear( true, false, false, 0, 0.0f, 0.0f, 0.0f, 0.0f );

			// the size of the box that will get the warped pixels
			// With the 7" displays, this will be less than half the screen width
			const int pixelDimensions = ( screenConfig.nativeScreenWidth >> 1 ) * stereoRender_warpTargetFraction.GetFloat();

			// Always scissor to the half-screen boundary, but the viewports
			// might cross that boundary if the lenses can be adjusted closer
			// together.
			renderBackend->Viewport( ( screenConfig.nativeScreenWidth >> 1 ) - pixelDimensions,
				( screenConfig.nativeScreenHeight >> 1 ) - ( pixelDimensions >> 1 ),
				pixelDimensions, pixelDimensions );
			renderBackend->Scissor( 0, 0, screenConfig.nativeScreenWidth >> 1, screenConfig.nativeScreenHeight );

			idVec4	color( stereoRender_warpCenterX.GetFloat(), stereoRender_warpCenterY.GetFloat(), stereoRender_warpParmZ.GetFloat(), stereoRender_warpParmW.GetFloat() );
			// don't use RB_SetColor(), because we don't want to clamp
			renderProgManager.SetRenderParm( RENDERPARM_COLOR, color.ToFloatPtr() );

			// outside the eye image the warp reads transparent black
			RB_SetStereoImageSampler( stereoRenderImages[0], TF_LINEAR, TR_CLAMP_TO_ZERO_ALPHA );
			stereoRenderImages[0]->Bind( 0 );
			RB_DrawElementsWithCounters( &backEnd.unitSquareSurface );

			idVec4	color2( stereoRender_warpCenterX.GetFloat(), stereoRender_warpCenterY.GetFloat(), stereoRender_warpParmZ.GetFloat(), stereoRender_warpParmW.GetFloat() );
			// don't use RB_SetColor(), because we don't want to clamp
			renderProgManager.SetRenderParm( RENDERPARM_COLOR, color2.ToFloatPtr() );

			renderBackend->Viewport( ( screenConfig.nativeScreenWidth >> 1 ),
				( screenConfig.nativeScreenHeight >> 1 ) - ( pixelDimensions >> 1 ),
				pixelDimensions, pixelDimensions );
			renderBackend->Scissor( screenConfig.nativeScreenWidth >> 1, 0, screenConfig.nativeScreenWidth >> 1, screenConfig.nativeScreenHeight );

			RB_SetStereoImageSampler( stereoRenderImages[1], TF_LINEAR, TR_CLAMP_TO_ZERO_ALPHA );
			stereoRenderImages[1]->Bind( 0 );
			RB_DrawElementsWithCounters( &backEnd.unitSquareSurface );
			break;
		}
		// a non-warped side-by-side-uncompressed (dual input cable) is rendered
		// just like STEREO3D_SIDE_BY_SIDE_COMPRESSED, so fall through.
	case STEREO3D_SIDE_BY_SIDE_COMPRESSED:
		stereoRenderImages[0]->Bind( 0 );
		stereoRenderImages[1]->Bind( 1 );
		RB_ViewportAndScissor( 0, 0, renderSystem->GetWidth(), renderSystem->GetHeight() );
		RB_DrawElementsWithCounters( &backEnd.unitSquareSurface );

		stereoRenderImages[1]->Bind( 0 );
		stereoRenderImages[0]->Bind( 1 );
		RB_ViewportAndScissor( renderSystem->GetWidth(), 0, renderSystem->GetWidth(), renderSystem->GetHeight() );
		RB_DrawElementsWithCounters( &backEnd.unitSquareSurface );
		break;

	case STEREO3D_TOP_AND_BOTTOM_COMPRESSED:
		stereoRenderImages[0]->Bind( 1 );
		stereoRenderImages[1]->Bind( 0 );
		RB_ViewportAndScissor( 0, 0, renderSystem->GetWidth(), renderSystem->GetHeight() );
		RB_DrawElementsWithCounters( &backEnd.unitSquareSurface );

		stereoRenderImages[1]->Bind( 1 );
		stereoRenderImages[0]->Bind( 0 );
		RB_ViewportAndScissor( 0, renderSystem->GetHeight(), renderSystem->GetWidth(), renderSystem->GetHeight() );
		RB_DrawElementsWithCounters( &backEnd.unitSquareSurface );
		break;

	case STEREO3D_INTERLACED:
		// every other scanline
		// Only image 0 is read unfiltered and image 1 stays linear, as the GL calls that stood here
		// did: the filter changes meant for both images reached the texture of unit 0.
		RB_SetStereoImageSampler( stereoRenderImages[0], TF_NEAREST, TR_CLAMP );
		stereoRenderImages[0]->Bind( 0 );
		stereoRenderImages[1]->Bind( 1 );

		RB_ViewportAndScissor( 0, 0, renderSystem->GetWidth(), renderSystem->GetHeight()*2 );
		renderProgManager.BindShader_StereoInterlace();
		RB_DrawElementsWithCounters( &backEnd.unitSquareSurface );

		// the image remembers linear, for the other stereo modes
		stereoRenderImages[0]->SetSamplerState( TF_LINEAR, TR_CLAMP );

		break;
	}

	RB_EndFrame();

	// we may choose to sync to the swapbuffers before the next frame

	// stop rendering on this thread
	uint64 backEndFinishTime = Sys_Microseconds();
	backEnd.pc.totalMicroSec = backEndFinishTime - backEndStartTime;
}

/*
====================
RB_ExecuteBackEndCommands

This function will be called syncronously if running without
smp extensions, or asyncronously by another thread.
====================
*/
void RB_ExecuteBackEndCommands( const emptyCommand_t *cmds ) {
	// r_debugRenderToTexture
	int c_draw3d = 0;
	int c_draw2d = 0;
	int c_setBuffers = 0;
	int c_copyRenders = 0;

	// a frame that traces no main view's shadows ends the front end's count on them (rtShadowLatch_t)
	backEnd.shadowLatch.BeginFrame();
	backEnd.tracedScene = {};

	// nothing to draw: no frame is opened
	if ( cmds->commandId == RC_NOP && !cmds->next ) {
		return;
	}

	renderBackend->BeginFrame();
	renderBackend->BeginDrawStream();

	resolutionScale.SetCurrentGPUFrameTime( commonLocal.GetRendererGPUMicroseconds() );

	renderLog.StartFrame();

	if ( renderSystem->GetStereo3DMode() != STEREO3D_OFF ) {
		RB_StereoRenderExecuteBackEndCommands( cmds );
		return;
	}

	uint64 backEndStartTime = Sys_Microseconds();

	// needed for editor rendering
	renderBackend->SetDefaultState();

	const drawSurfsCommand_t * const hdrView = RB_FindHdrMainView( cmds, 0 );
	RB_BeginHdrScene( hdrView );
	for ( ; cmds != NULL; cmds = (const emptyCommand_t *)cmds->next ) {
		switch ( cmds->commandId ) {
		case RC_NOP:
			break;
		case RC_DRAW_VIEW_3D:
		case RC_DRAW_VIEW_GUI:
			RB_DrawView( cmds, 0, cmds == static_cast<const void *>( hdrView ) );
			if ( cmds == static_cast<const void *>( hdrView ) ) {
				RB_EndHdrScene();
			}
			if ( ((const drawSurfsCommand_t *)cmds)->viewDef->viewEntitys ) {
				c_draw3d++;
			} else {
				c_draw2d++;
			}
			break;
		case RC_SET_BUFFER:
			RB_SetBuffer( false );
			c_setBuffers++;
			break;
		case RC_COPY_RENDER:
			RB_CopyRender( cmds );
			c_copyRenders++;
			break;
		case RC_POST_PROCESS:
			RB_PostProcess( cmds );
			break;
		default:
			common->Error( "RB_ExecuteBackEndCommands: bad commandId" );
			break;
		}
	}

	RB_EndFrame();

	// stop rendering on this thread
	uint64 backEndFinishTime = Sys_Microseconds();
	backEnd.pc.totalMicroSec = backEndFinishTime - backEndStartTime;

	if ( r_debugRenderToTexture.GetInteger() == 1 ) {
		common->Printf( "3d: %i, 2d: %i, SetBuf: %i, CpyRenders: %i, CpyFrameBuf: %i\n", c_draw3d, c_draw2d, c_setBuffers, c_copyRenders, backEnd.pc.c_copyFrameBuffer );
		backEnd.pc.c_copyFrameBuffer = 0;
	}
}
