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
#include "ConsoleHistory.h"
#include "../renderer/ResolutionScale.h"
#include "Common_local.h"
#include "../renderer/Font.h"

#define	CON_TEXTSIZE			0x30000
#define	NUM_CON_TIMES			4
#define CONSOLE_FIRSTREPEAT		200
#define CONSOLE_REPEAT			16		// the old rate: it repeated once per drawn frame at 60 Hz

#define	COMMAND_HISTORY			64

struct overlayText_t {
	idStr			text;
	justify_t		justify;
	int				time;
};

// the console will query the cvar and command systems for
// command completion information

class idConsoleLocal : public idConsole {
public:
	virtual	void		Init();
	virtual void		Shutdown();
	virtual	bool		ProcessEvent( const sysEvent_t * event, bool forceAccept );
	virtual	bool		Active();
	virtual	void		ClearNotifyLines();
	virtual	void		Close();
	virtual	void		Print( const char *text );
	virtual	void		Draw( bool forceFullScreen );

	virtual void		PrintOverlay( idOverlayHandle &handle, justify_t justify, const char *text, ... );

	virtual idDebugGraph *	CreateGraph( int numItems );
	virtual void			DestroyGraph( idDebugGraph * graph );

	void				Dump( const char *toFile );
	void				Clear();

private:
	void				KeyDownEvent( int key );

	void				Linefeed();

	void				PageUp();
	void				PageDown();
	void				Top();
	void				Bottom();

	void				DrawInput();
	void				DrawNotify();
	void				DrawSolidConsole( float frac );

	void				Scroll();
	void				SetDisplayFraction( float frac );
	void				UpdateDisplayFraction();

	void				DrawTextLeftAlign( float x, float &y, const char *text, ... );
	void				DrawTextRightAlign( float x, float &y, const char *text, ... );

	float				DrawFPS( float y );
	float				DrawMemoryUsage( float y );

	void				DrawOverlayText( float & leftY, float & rightY, float & centerY );
	void				DrawDebugGraphs();

	//============================

	// allow these constants to be adjusted for HMD
	int					LOCALSAFE_LEFT;
	int					LOCALSAFE_RIGHT;
	int					LOCALSAFE_TOP;
	int					LOCALSAFE_BOTTOM;
	int					LOCALSAFE_WIDTH;
	int					LOCALSAFE_HEIGHT;
	int					LINE_WIDTH;
	int					TOTAL_LINES;

	bool				keyCatching;

	short				text[CON_TEXTSIZE];
	int					current;		// line where next message will be printed
	int					x;				// offset in current line for next print
	int					display;		// bottom of console displays this line
	int					lastKeyEvent;	// time of last key event for scroll delay
	int					nextKeyEvent;	// keyboard repeat rate

	float				displayFrac;	// approaches finalFrac at con_speed
	float				finalFrac;		// 0.0 to 1.0 lines of console to display
	int					fracTime;		// time of last displayFrac update

	int					vislines;		// in scanlines

	int					times[NUM_CON_TIMES];	// cls.realtime time the line was generated
									// for transparent notify lines
	idVec4				color;

	idEditField			historyEditLines[COMMAND_HISTORY];

	int					nextHistoryLine;// the last line in the history buffer, not masked
	int					historyLine;	// the line being displayed from history buffer
									// will be <= nextHistoryLine

	idEditField			consoleField;

	idList< overlayText_t >	overlayText;
	idList< idDebugGraph *> debugGraphs;

	static idCVar		con_speed;
	static idCVar		con_notifyTime;
	static idCVar		con_noPrint;
};

static idConsoleLocal localConsole;
idConsole * console = &localConsole;

idCVar idConsoleLocal::con_speed( "con_speed", "3", CVAR_SYSTEM, "speed at which the console moves up and down" );
idCVar idConsoleLocal::con_notifyTime( "con_notifyTime", "3", CVAR_SYSTEM, "time messages are displayed onscreen when console is pulled up" );
#ifdef DEBUG
idCVar idConsoleLocal::con_noPrint( "con_noPrint", "0", CVAR_BOOL|CVAR_SYSTEM|CVAR_NOCHEAT, "print on the console but not onscreen when console is pulled up" );
#else
idCVar idConsoleLocal::con_noPrint( "con_noPrint", "1", CVAR_BOOL|CVAR_SYSTEM|CVAR_NOCHEAT, "print on the console but not onscreen when console is pulled up" );
#endif

/*
=============================================================================

	Misc stats

=============================================================================
*/

/*
==================
idConsoleLocal::DrawTextLeftAlign
==================
*/
void idConsoleLocal::DrawTextLeftAlign( float x, float &y, const char *text, ... ) {
	char string[MAX_STRING_CHARS];
	va_list argptr;
	va_start( argptr, text );
	idStr::vsnPrintf( string, sizeof( string ), text, argptr );
	va_end( argptr );
	renderSystem->DrawSmallStringExt( x, y + 2, string, colorWhite, true );
	y += SMALLCHAR_HEIGHT + 4;
}

/*
==================
idConsoleLocal::DrawTextRightAlign
==================
*/
void idConsoleLocal::DrawTextRightAlign( float x, float &y, const char *text, ... ) {
	char string[MAX_STRING_CHARS];
	va_list argptr;
	va_start( argptr, text );
	int i = idStr::vsnPrintf( string, sizeof( string ), text, argptr );
	va_end( argptr );
	renderSystem->DrawSmallStringExt( x - i * SMALLCHAR_WIDTH, y + 2, string, colorWhite, true );
	y += SMALLCHAR_HEIGHT + 4;
}




/*
=============================================================================

	Performance overlay (com_showFPS)

	Drawn in real screen pixels (converted to the virtual 640x480 space),
	so it keeps its proportions at any resolution and aspect ratio.

=============================================================================
*/

idCVar com_showFPSScale( "com_showFPSScale", "1", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_FLOAT, "size of the com_showFPS overlay", 0.5f, 3.0f );

static const int	PERF_GRAPH_FRAMES		= 120;		// frame time history shown in the graph
static const int	PERF_REFRESH_MSEC		= 250;		// how often the numbers are refreshed

static const idVec4	perfColorPanel( 0.02f, 0.02f, 0.03f, 0.72f );
static const idVec4	perfColorGraphBg( 1.0f, 1.0f, 1.0f, 0.05f );
static const idVec4	perfColorSeparator( 1.0f, 1.0f, 1.0f, 0.10f );
static const idVec4	perfColorText( 1.0f, 1.0f, 1.0f, 1.0f );
static const idVec4	perfColorLabel( 0.68f, 0.70f, 0.74f, 1.0f );
static const idVec4	perfColorShadow( 0.0f, 0.0f, 0.0f, 0.55f );
static const idVec4	perfColorBudget( 1.0f, 1.0f, 1.0f, 0.35f );
static const idVec4	perfColorGood( 0.40f, 0.88f, 0.50f, 1.0f );
static const idVec4	perfColorWarn( 1.0f, 0.78f, 0.28f, 1.0f );
static const idVec4	perfColorBad( 1.0f, 0.34f, 0.30f, 1.0f );

/*
========================
idPerfOverlayPainter

Small helper that draws boxes and text with a real font in screen pixels.
========================
*/
class idPerfOverlayPainter {
public:
	void Begin() {
		const float w = (float)Max( 1, renderSystem->GetWidth() );
		const float h = (float)Max( 1, renderSystem->GetHeight() );
		screenWidth = w;
		toVirtualX = SCREEN_WIDTH / w;
		toVirtualY = SCREEN_HEIGHT / h;
		ui = ( h / 1080.0f ) * com_showFPSScale.GetFloat();
		font = renderSystem->RegisterFont( "Arial_Narrow" );
	}

	float	UI( float referencePixels ) const { return referencePixels * ui; }
	float	ScreenWidth() const { return screenWidth; }
	float	ToVirtualY( float y ) const { return y * toVirtualY; }

	void Fill( float x, float y, float w, float h, const idVec4 & color ) const {
		if ( w <= 0.0f || h <= 0.0f ) {
			return;
		}
		renderSystem->DrawFilled( color, x * toVirtualX, y * toVirtualY, w * toVirtualX, h * toVirtualY );
	}

	float TextWidth( const char * text, float size ) const {
		const float scale = size * ui / 48.0f;
		float w = 0.0f;
		for ( const char * c = text; *c != 0; c++ ) {
			w += font->GetGlyphWidth( scale, (unsigned char)*c );
		}
		return w;
	}

	// y is the top of the text line; returns the width of the text
	float Text( float x, float y, float size, const idVec4 & color, const char * text, bool alignRight = false ) const {
		const float width = TextWidth( text, size );
		if ( alignRight ) {
			x -= width;
		}
		const float shadow = Max( 1.0f, idMath::Floor( ui + 0.5f ) );
		DrawString( x + shadow, y + shadow, size, perfColorShadow, text );
		DrawString( x, y, size, color, text );
		return width;
	}

private:
	void DrawString( float x, float y, float size, const idVec4 & color, const char * text ) const {
		const float scale = size * ui / 48.0f;
		const float baseline = idMath::Floor( y + size * ui * 0.82f + 0.5f );
		renderSystem->SetColor( color );
		float penX = x;
		for ( const char * c = text; *c != 0; c++ ) {
			scaledGlyphInfo_t glyph;
			font->GetScaledGlyph( scale, (unsigned char)*c, glyph );
			if ( glyph.material != NULL && glyph.width > 0.0f ) {
				const float gx = idMath::Floor( penX + glyph.left + 0.5f );
				const float gy = baseline - glyph.top;
				renderSystem->DrawStretchPic( gx * toVirtualX, gy * toVirtualY, glyph.width * toVirtualX, glyph.height * toVirtualY,
					glyph.s1, glyph.t1, glyph.s2, glyph.t2, glyph.material );
			}
			penX += glyph.xSkip;
		}
		renderSystem->SetColor( colorWhite );
	}

	idFont *	font;
	float		screenWidth;
	float		toVirtualX;
	float		toVirtualY;
	float		ui;
};

/*
========================
PerfFormatCount

1234567 -> "1 234 567"
========================
*/
static const char * PerfFormatCount( int value ) {
	static char buffers[4][32];
	static int next;
	char * out = buffers[next++ & 3];
	char digits[16];
	idStr::snPrintf( digits, sizeof( digits ), "%d", Max( 0, value ) );
	const int len = idStr::Length( digits );
	int o = 0;
	for ( int i = 0; i < len; i++ ) {
		if ( i > 0 && ( len - i ) % 3 == 0 ) {
			out[o++] = ' ';
		}
		out[o++] = digits[i];
	}
	out[o] = 0;
	return out;
}

/*
========================
PerfStatusColor

ratio = time / frame budget
========================
*/
static const idVec4 & PerfStatusColor( float ratio ) {
	if ( ratio <= 1.05f ) {
		return perfColorGood;
	}
	if ( ratio <= 1.6f ) {
		return perfColorWarn;
	}
	return perfColorBad;
}

enum perfStat_t {
	PERF_FRAME,
	PERF_GAME,
	PERF_FRONTEND,
	PERF_BACKEND,
	PERF_SHADOWS,
	PERF_GPU,
	PERF_SYNC,
	PERF_NUM_STATS
};

/*
==================
idConsoleLocal::DrawFPS
==================
*/
float idConsoleLocal::DrawFPS( float y ) {
	// frame time history, sampled from the main thread frame timing
	static float	frameHistory[PERF_GRAPH_FRAMES];
	static int		frameHistoryCount;
	static uint64	lastFrameStart;

	// accumulated and displayed (averaged) values
	static double	accum[PERF_NUM_STATS];
	static double	accumMax;
	static int		accumFrames;
	static int		lastRefreshTime;
	static float	shown[PERF_NUM_STATS];
	static float	shownMax;

	const frameTiming_t & timing = commonLocal.mainFrameTiming;
	if ( timing.startSyncTime != lastFrameStart ) {
		if ( lastFrameStart != 0 && timing.startSyncTime > lastFrameStart ) {
			const float frameMs = ( timing.startSyncTime - lastFrameStart ) / 1000.0f;
			frameHistory[frameHistoryCount % PERF_GRAPH_FRAMES] = frameMs;
			frameHistoryCount++;

			accum[PERF_FRAME] += frameMs;
			accum[PERF_GAME] += ( timing.finishGameTime - timing.startGameTime ) / 1000.0;
			accum[PERF_FRONTEND] += ( timing.finishDrawTime - timing.finishGameTime ) / 1000.0;
			accum[PERF_BACKEND] += commonLocal.GetRendererBackEndMicroseconds() / 1000.0;
			accum[PERF_SHADOWS] += commonLocal.GetRendererShadowsMicroseconds() / 1000.0;
			accum[PERF_GPU] += commonLocal.GetRendererGPUMicroseconds() / 1000.0;
			accum[PERF_SYNC] += ( timing.finishSyncTime - timing.startSyncTime ) / 1000.0;
			accumMax = Max( accumMax, (double)frameMs );
			accumFrames++;
		}
		lastFrameStart = timing.startSyncTime;
	}

	const int now = Sys_Milliseconds();
	if ( accumFrames > 0 && ( now - lastRefreshTime >= PERF_REFRESH_MSEC || lastRefreshTime == 0 ) ) {
		for ( int i = 0; i < PERF_NUM_STATS; i++ ) {
			shown[i] = (float)( accum[i] / accumFrames );
			accum[i] = 0.0;
		}
		shownMax = (float)accumMax;
		accumMax = 0.0;
		accumFrames = 0;
		lastRefreshTime = now;
	}

	int numDraws = 0;
	int numTris = 0;
	int numShadowTris = 0;
	renderSystem->GetLastFrameDrawStats( numDraws, numTris, numShadowTris );

	// frames no longer wait for a tic, so the budget is the frame rate the loop aims for
	const float targetHz = Max( 1.0f, commonLocal.GetTargetFrameRate() );
	const float budgetMs = 1000.0f / targetHz;

	idPerfOverlayPainter p;
	p.Begin();

	// layout in screen pixels (reference values are for a 1080p screen)
	const float pad = p.UI( 14 );
	const float panelW = p.UI( 270 );
	const float panelX = p.ScreenWidth() - p.UI( 16 ) - panelW;
	const float panelY = p.UI( 16 );
	const float innerX = panelX + pad;
	const float innerW = panelW - pad * 2.0f;
	const float innerR = innerX + innerW;
	const float rowH = p.UI( 21 );
	const float graphH = p.UI( 46 );
	const float lineH = Max( 1.0f, p.UI( 1 ) );

	idStr resolutionText;
	resolutionScale.GetConsoleText( resolutionText );
	const bool showResolution = resolutionText.Icmp( "rs-off" ) != 0;

	// with r_pix 1, the render passes with the most GPU time
	static const int MAX_PASS_ROWS = 3;
	renderPassTiming_t passes[MAX_PASS_ROWS];
	const int numPassRows = renderSystem->GetRenderPassTimings( passes );
	const float passRowH = p.UI( 18 );

	// render interpolation (r_interpolate), when the last frame used it
	renderInterpStats_t interp;
	renderSystem->GetLastFrameInterpStats( interp );
	idStr interpText;
	idStr interpMoreText;
	if ( interp.active ) {
		interpText = va( "%d/%d  snap %d  a %.2f", interp.entities, interp.listed, interp.snapped, interp.alpha );
		if ( interp.attached + interp.lights + interp.skeletons > 0 ) {
			interpMoreText = va( "attached %d  lights %d  skel %d", interp.attached, interp.lights, interp.skeletons );
		}
		if ( interp.lateYaw != 0.0f || interp.latePitch != 0.0f ) {
			interpMoreText += va( "%slate %.1f %.1f", interpMoreText.IsEmpty() ? "" : "  ", interp.lateYaw, interp.latePitch );
		}
	}

	static const int NUM_TIMING_ROWS = 6;
	const int numCountRows = ( showResolution ? 3 : 2 ) + ( interp.active ? 1 : 0 ) + ( interpMoreText.IsEmpty() ? 0 : 1 );
	const float panelH = pad
		+ p.UI( 48 )						// fps header
		+ graphH + p.UI( 6 )				// graph
		+ rowH + p.UI( 10 )					// avg / max line + separator
		+ NUM_TIMING_ROWS * rowH + p.UI( 10 )	// timings + separator
		+ ( numPassRows > 0 ? numPassRows * passRowH + p.UI( 10 ) : 0.0f )	// GPU passes + separator
		+ numCountRows * rowH
		+ pad - p.UI( 4 );

	const idVec4 & frameColor = PerfStatusColor( shown[PERF_FRAME] / budgetMs );

	p.Fill( panelX, panelY, panelW, panelH, perfColorPanel );
	p.Fill( panelX, panelY, panelW, Max( 1.0f, p.UI( 2 ) ), frameColor );

	float cy = panelY + pad;

	// big FPS number + frame time
	const float fps = shown[PERF_FRAME] > 0.0f ? 1000.0f / shown[PERF_FRAME] : 0.0f;
	const float fpsW = p.Text( innerX, cy - p.UI( 4 ), 40, frameColor, va( "%d", idMath::Ftoi( fps + 0.5f ) ) );
	p.Text( innerX + fpsW + p.UI( 6 ), cy + p.UI( 15 ), 16, perfColorLabel, "FPS" );
	p.Text( innerR, cy + p.UI( 1 ), 20, perfColorText, va( "%.2f ms", shown[PERF_FRAME] ), true );
	p.Text( innerR, cy + p.UI( 23 ), 13, perfColorLabel, va( "target %d Hz", idMath::Ftoi( targetHz + 0.5f ) ), true );
	cy += p.UI( 48 );

	// frame time graph, newest frame on the right
	p.Fill( innerX, cy, innerW, graphH, perfColorGraphBg );
	const float graphMaxMs = budgetMs * 2.5f;
	const int numBars = Min( frameHistoryCount, PERF_GRAPH_FRAMES );
	const float barW = innerW / PERF_GRAPH_FRAMES;
	float windowMax = 0.0f;
	for ( int i = 0; i < numBars; i++ ) {
		const float ms = frameHistory[( frameHistoryCount - 1 - i ) % PERF_GRAPH_FRAMES];
		windowMax = Max( windowMax, ms );
		const float h = Max( lineH, graphH * Min( ms / graphMaxMs, 1.0f ) );
		const float bx = innerR - ( i + 1 ) * barW;
		idVec4 graphColor = PerfStatusColor( ms / budgetMs );
		graphColor.w = 0.85f;
		p.Fill( bx, cy + graphH - h, Max( 1.0f, barW - lineH * 0.5f ), h, graphColor );
	}
	const float budgetY = cy + graphH - graphH * ( budgetMs / graphMaxMs );
	p.Fill( innerX, budgetY, innerW, lineH, perfColorBudget );
	cy += graphH + p.UI( 6 );

	p.Text( innerX, cy, 14, perfColorLabel, va( "avg %.2f", shown[PERF_FRAME] ) );
	p.Text( innerX + innerW * 0.34f, cy, 14, shownMax > budgetMs * 1.6f ? perfColorBad : perfColorLabel, va( "max %.1f", shownMax ) );
	p.Text( innerR, cy, 14, windowMax > budgetMs * 1.6f ? perfColorBad : perfColorLabel, va( "graph max %.1f ms", windowMax ), true );
	cy += rowH + p.UI( 4 );

	p.Fill( innerX, cy, innerW, lineH, perfColorSeparator );
	cy += p.UI( 6 );

	// timing breakdown with small bars relative to the frame budget
	struct timingRow_t {
		const char *	label;
		perfStat_t		stat;
	};
	static const timingRow_t rows[NUM_TIMING_ROWS] = {
		{ "Game logic",		PERF_GAME },
		{ "Render front",	PERF_FRONTEND },
		{ "Render back",	PERF_BACKEND },
		{ "Shadows",		PERF_SHADOWS },
		{ "GPU",			PERF_GPU },
		{ "GPU wait",		PERF_SYNC },
	};
	const float barX = innerX + p.UI( 96 );
	const float barMaxW = innerW - p.UI( 96 ) - p.UI( 58 );
	for ( int i = 0; i < NUM_TIMING_ROWS; i++ ) {
		const float ms = shown[rows[i].stat];
		p.Text( innerX, cy, 16, perfColorLabel, rows[i].label );
		p.Fill( barX, cy + p.UI( 8 ), barMaxW, p.UI( 5 ), perfColorGraphBg );
		idVec4 barColor = PerfStatusColor( ms / budgetMs );
		barColor.w = 0.9f;
		p.Fill( barX, cy + p.UI( 8 ), barMaxW * Min( ms / budgetMs, 1.0f ), p.UI( 5 ), barColor );
		p.Text( innerR, cy, 16, perfColorText, va( "%.2f", ms ), true );
		cy += rowH;
	}
	cy += p.UI( 4 );

	p.Fill( innerX, cy, innerW, lineH, perfColorSeparator );
	cy += p.UI( 6 );

	if ( numPassRows > 0 ) {
		for ( int i = 0; i < numPassRows; i++ ) {
			p.Text( innerX, cy, 14, perfColorLabel, va( "GPU  %s", passes[i].name ) );
			p.Text( innerR, cy, 14, perfColorText, va( "%.2f", passes[i].gpuMilliseconds ), true );
			cy += passRowH;
		}
		cy += p.UI( 4 );

		p.Fill( innerX, cy, innerW, lineH, perfColorSeparator );
		cy += p.UI( 6 );
	}

	// scene counters
	p.Text( innerX, cy, 16, perfColorLabel, "Draw calls" );
	p.Text( innerR, cy, 16, perfColorText, PerfFormatCount( numDraws ), true );
	cy += rowH;
	p.Text( innerX, cy, 16, perfColorLabel, "Triangles" );
	p.Text( innerR, cy, 16, perfColorText, va( "%s  (%s shadow)", PerfFormatCount( numTris ), PerfFormatCount( numShadowTris ) ), true );
	cy += rowH;
	if ( showResolution ) {
		p.Text( innerX, cy, 16, perfColorLabel, "Resolution scale" );
		p.Text( innerR, cy, 16, perfColorText, resolutionText.c_str(), true );
		cy += rowH;
	}
	if ( interp.active ) {
		p.Text( innerX, cy, 16, perfColorLabel, "Interpolated" );
		p.Text( innerR, cy, 14, perfColorText, interpText.c_str(), true );
		cy += rowH;
	}
	if ( !interpMoreText.IsEmpty() ) {
		p.Text( innerR, cy, 14, perfColorText, interpMoreText.c_str(), true );
		cy += rowH;
	}

	return p.ToVirtualY( panelY + panelH ) + 8.0f;
}

/*
==================
idConsoleLocal::DrawMemoryUsage
==================
*/
float idConsoleLocal::DrawMemoryUsage( float y ) {
	return y;
}

//=========================================================================

/*
==============
Con_Clear_f
==============
*/
static void Con_Clear_f( const idCmdArgs &args ) {
	localConsole.Clear();
}

/*
==============
Con_Dump_f
==============
*/
static void Con_Dump_f( const idCmdArgs &args ) {
	if ( args.Argc() != 2 ) {
		common->Printf( "usage: conDump <filename>\n" );
		return;
	}

	idStr fileName = args.Argv(1);
	fileName.DefaultFileExtension(".txt");

	common->Printf( "Dumped console text to %s.\n", fileName.c_str() );

	localConsole.Dump( fileName.c_str() );
}

/*
==============
idConsoleLocal::Init
==============
*/
void idConsoleLocal::Init() {
	int		i;

	keyCatching = false;

	LOCALSAFE_LEFT		= 32;
	LOCALSAFE_RIGHT		= 608;
	LOCALSAFE_TOP		= 24;
	LOCALSAFE_BOTTOM	= 456;
	LOCALSAFE_WIDTH		= LOCALSAFE_RIGHT - LOCALSAFE_LEFT;
	LOCALSAFE_HEIGHT	= LOCALSAFE_BOTTOM - LOCALSAFE_TOP;

	LINE_WIDTH = ( ( LOCALSAFE_WIDTH / SMALLCHAR_WIDTH ) - 2 );
	TOTAL_LINES = (CON_TEXTSIZE / LINE_WIDTH);

	lastKeyEvent = -1;
	nextKeyEvent = CONSOLE_FIRSTREPEAT;

	consoleField.Clear();
	consoleField.SetWidthInChars( LINE_WIDTH );

	for ( i = 0 ; i < COMMAND_HISTORY ; i++ ) {
		historyEditLines[i].Clear();
		historyEditLines[i].SetWidthInChars( LINE_WIDTH );
	}

	cmdSystem->AddCommand( "clear", Con_Clear_f, CMD_FL_SYSTEM, "clears the console" );
	cmdSystem->AddCommand( "conDump", Con_Dump_f, CMD_FL_SYSTEM, "dumps the console text to a file" );
}

/*
==============
idConsoleLocal::Shutdown
==============
*/
void idConsoleLocal::Shutdown() {
	cmdSystem->RemoveCommand( "clear" );
	cmdSystem->RemoveCommand( "conDump" );

	debugGraphs.DeleteContents( true );
}

/*
================
idConsoleLocal::Active
================
*/
bool	idConsoleLocal::Active() {
	return keyCatching;
}

/*
================
idConsoleLocal::ClearNotifyLines
================
*/
void	idConsoleLocal::ClearNotifyLines() {
	int		i;

	for ( i = 0 ; i < NUM_CON_TIMES ; i++ ) {
		times[i] = 0;
	}
}

/*
================
idConsoleLocal::Close
================
*/
void	idConsoleLocal::Close() {
	keyCatching = false;
	SetDisplayFraction( 0 );
	displayFrac = 0;	// don't scroll to that point, go immediately
	ClearNotifyLines();
}

/*
================
idConsoleLocal::Clear
================
*/
void idConsoleLocal::Clear() {
	int		i;

	for ( i = 0 ; i < CON_TEXTSIZE ; i++ ) {
		text[i] = (idStr::ColorIndex(C_COLOR_CYAN)<<8) | ' ';
	}

	Bottom();		// go to end
}

/*
================
idConsoleLocal::Dump

Save the console contents out to a file
================
*/
void idConsoleLocal::Dump( const char *fileName ) {
	int		l, x, i;
	short *	line;
	idFile *f;
	char	* buffer = (char *)alloca( LINE_WIDTH + 3 );

	f = fileSystem->OpenFileWrite( fileName );
	if ( !f ) {
		common->Warning( "couldn't open %s", fileName );
		return;
	}

	// skip empty lines
	l = current - TOTAL_LINES + 1;
	if ( l < 0 ) {
		l = 0;
	}
	for ( ; l <= current ; l++ )
	{
		line = text + ( l % TOTAL_LINES ) * LINE_WIDTH;
		for ( x = 0; x < LINE_WIDTH; x++ )
			if ( ( line[x] & 0xff ) > ' ' )
				break;
		if ( x != LINE_WIDTH )
			break;
	}

	// write the remaining lines
	for ( ; l <= current; l++ ) {
		line = text + ( l % TOTAL_LINES ) * LINE_WIDTH;
		for( i = 0; i < LINE_WIDTH; i++ ) {
			buffer[i] = line[i] & 0xff;
		}
		for ( x = LINE_WIDTH-1; x >= 0; x-- ) {
			if ( buffer[x] <= ' ' ) {
				buffer[x] = 0;
			} else {
				break;
			}
		}
		buffer[x+1] = '\r';
		buffer[x+2] = '\n';
		buffer[x+3] = 0;
		f->Write( buffer, strlen( buffer ) );
	}

	fileSystem->CloseFile( f );
}

/*
================
idConsoleLocal::PageUp
================
*/
void idConsoleLocal::PageUp() {
	display -= 2;
	if ( current - display >= TOTAL_LINES ) {
		display = current - TOTAL_LINES + 1;
	}
}

/*
================
idConsoleLocal::PageDown
================
*/
void idConsoleLocal::PageDown() {
	display += 2;
	if ( display > current ) {
		display = current;
	}
}

/*
================
idConsoleLocal::Top
================
*/
void idConsoleLocal::Top() {
	display = 0;
}

/*
================
idConsoleLocal::Bottom
================
*/
void idConsoleLocal::Bottom() {
	display = current;
}


/*
=============================================================================

CONSOLE LINE EDITING

==============================================================================
*/

/*
====================
KeyDownEvent

Handles history and console scrollback
====================
*/
void idConsoleLocal::KeyDownEvent( int key ) {
	
	// Execute F key bindings
	if ( key >= K_F1 && key <= K_F12 ) {
		idKeyInput::ExecKeyBinding( key );
		return;
	}

	// ctrl-L clears screen
	if ( key == K_L && ( idKeyInput::IsDown( K_LCTRL ) || idKeyInput::IsDown( K_RCTRL ) ) ) {
		Clear();
		return;
	}

	// enter finishes the line
	if ( key == K_ENTER || key == K_KP_ENTER ) {

		common->Printf ( "]%s\n", consoleField.GetBuffer() );

		cmdSystem->BufferCommandText( CMD_EXEC_APPEND, consoleField.GetBuffer() );	// valid command
		cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "\n" );

		// copy line to history buffer

		if ( consoleField.GetBuffer()[ 0 ] != '\n' && consoleField.GetBuffer()[ 0 ] != '\0' ) {
			consoleHistory.AddToHistory( consoleField.GetBuffer() );
		}

		consoleField.Clear();
		consoleField.SetWidthInChars( LINE_WIDTH );

		const bool captureToImage = false;
		common->UpdateScreen( captureToImage );// force an update, because the command
								// may take some time
		return;
	}

	// command completion

	if ( key == K_TAB ) {
		consoleField.AutoComplete();
		return;
	}

	// command history (ctrl-p ctrl-n for unix style)

	if ( ( key == K_UPARROW ) ||
		 ( key == K_P && ( idKeyInput::IsDown( K_LCTRL ) || idKeyInput::IsDown( K_RCTRL ) ) ) ) {
		idStr hist = consoleHistory.RetrieveFromHistory( true );
		if ( !hist.IsEmpty() ) {
			consoleField.SetBuffer( hist );
		}
		return;
	}

	if ( ( key == K_DOWNARROW ) ||
		 ( key == K_N && ( idKeyInput::IsDown( K_LCTRL ) || idKeyInput::IsDown( K_RCTRL ) ) ) ) {
		idStr hist = consoleHistory.RetrieveFromHistory( false );
		if ( !hist.IsEmpty() ) {
			consoleField.SetBuffer( hist );
		}
		return;
	}

	// console scrolling
	if ( key == K_PGUP ) {
		PageUp();
		lastKeyEvent = eventLoop->Milliseconds();
		nextKeyEvent = CONSOLE_FIRSTREPEAT;
		return;
	}

	if ( key == K_PGDN ) {
		PageDown();
		lastKeyEvent = eventLoop->Milliseconds();
		nextKeyEvent = CONSOLE_FIRSTREPEAT;
		return;
	}

	if ( key == K_MWHEELUP ) {
		PageUp();
		return;
	}

	if ( key == K_MWHEELDOWN ) {
		PageDown();
		return;
	}

	// ctrl-home = top of console
	if ( key == K_HOME && ( idKeyInput::IsDown( K_LCTRL ) || idKeyInput::IsDown( K_RCTRL ) ) ) {
		Top();
		return;
	}

	// ctrl-end = bottom of console
	if ( key == K_END && ( idKeyInput::IsDown( K_LCTRL ) || idKeyInput::IsDown( K_RCTRL ) ) ) {
		Bottom();
		return;
	}

	// pass to the normal editline routine
	consoleField.KeyDownEvent( key );
}

/*
==============
Scroll
deals with scrolling text because we don't have key repeat
==============
*/
void idConsoleLocal::Scroll( ) {
	// Draw calls this every frame, so the repeat has to be timed
	const int now = eventLoop->Milliseconds();
	if ( lastKeyEvent == -1 || now < lastKeyEvent + nextKeyEvent ) {
		return;
	}
	// console scrolling
	if ( idKeyInput::IsDown( K_PGUP ) ) {
		PageUp();
		lastKeyEvent = now;
		nextKeyEvent = CONSOLE_REPEAT;
		return;
	}

	if ( idKeyInput::IsDown( K_PGDN ) ) {
		PageDown();
		lastKeyEvent = now;
		nextKeyEvent = CONSOLE_REPEAT;
		return;
	}
}

/*
==============
SetDisplayFraction

Causes the console to start opening the desired amount.
==============
*/
void idConsoleLocal::SetDisplayFraction( float frac ) {
	finalFrac = frac;
	fracTime = Sys_Milliseconds();
}

/*
==============
UpdateDisplayFraction

Scrolls the console up or down based on conspeed
==============
*/
void idConsoleLocal::UpdateDisplayFraction() {
	if ( con_speed.GetFloat() <= 0.1f ) {
		fracTime = Sys_Milliseconds();
		displayFrac = finalFrac;
		return;
	}

	// scroll towards the destination height
	if ( finalFrac < displayFrac ) {
		displayFrac -= con_speed.GetFloat() * ( Sys_Milliseconds() - fracTime ) * 0.001f;
		if ( finalFrac > displayFrac ) {
			displayFrac = finalFrac;
		}
		fracTime = Sys_Milliseconds();
	} else if ( finalFrac > displayFrac ) {
		displayFrac += con_speed.GetFloat() * ( Sys_Milliseconds() - fracTime ) * 0.001f;
		if ( finalFrac < displayFrac ) {
			displayFrac = finalFrac;
		}
		fracTime = Sys_Milliseconds();
	}
}

/*
==============
ProcessEvent
==============
*/
bool	idConsoleLocal::ProcessEvent( const sysEvent_t *event, bool forceAccept ) {
	const bool consoleKey = event->evType == SE_KEY && event->evValue == K_GRAVE && com_allowConsole.GetBool();

	// we always catch the console key event
	if ( !forceAccept && consoleKey ) {
		// ignore up events
		if ( event->evValue2 == 0 ) {
			return true;
		}

		consoleField.ClearAutoComplete();

		// a down event will toggle the destination lines
		if ( keyCatching ) {
			Close();
			Sys_GrabMouseCursor( true );
		} else {
			consoleField.Clear();
			keyCatching = true;
			if ( idKeyInput::IsDown( K_LSHIFT ) || idKeyInput::IsDown( K_RSHIFT ) ) {
				// if the shift key is down, don't open the console as much
				SetDisplayFraction( 0.2f );
			} else {
				SetDisplayFraction( 0.5f );
			}
		}
		return true;
	}

	// if we aren't key catching, dump all the other events
	if ( !forceAccept && !keyCatching ) {
		return false;
	}

	// handle key and character events
	if ( event->evType == SE_CHAR ) {
		// never send the console key as a character
		if ( event->evValue != '`' && event->evValue != '~' ) {
			consoleField.CharEvent( event->evValue );
		}
		return true;
	}

	if ( event->evType == SE_KEY ) {
		// ignore up key events
		if ( event->evValue2 == 0 ) {
			return true;
		}

		KeyDownEvent( event->evValue );
		return true;
	}

	// we don't handle things like mouse, joystick, and network packets
	return false;
}

/*
==============================================================================

PRINTING

==============================================================================
*/

/*
===============
Linefeed
===============
*/
void idConsoleLocal::Linefeed() {
	int		i;

	// mark time for transparent overlay
	if ( current >= 0 ) {
		times[current % NUM_CON_TIMES] = Sys_Milliseconds();
	}

	x = 0;
	if ( display == current ) {
		display++;
	}
	current++;
	for ( i = 0; i < LINE_WIDTH; i++ ) {
		int offset = ( (unsigned int)current % TOTAL_LINES ) * LINE_WIDTH + i;
		text[offset] = (idStr::ColorIndex(C_COLOR_CYAN)<<8) | ' ';
	}
}


/*
================
Print

Handles cursor positioning, line wrapping, etc
================
*/
void idConsoleLocal::Print( const char *txt ) {
	int		y;
	int		c, l;
	int		color;

	if ( TOTAL_LINES == 0 ) {
		// not yet initialized
		return;
	}

	color = idStr::ColorIndex( C_COLOR_CYAN );

	while ( (c = *(const unsigned char*)txt) != 0 ) {
		if ( idStr::IsColor( txt ) ) {
			if ( *(txt+1) == C_COLOR_DEFAULT ) {
				color = idStr::ColorIndex( C_COLOR_CYAN );
			} else {
				color = idStr::ColorIndex( *(txt+1) );
			}
			txt += 2;
			continue;
		}

		y = current % TOTAL_LINES;

		// if we are about to print a new word, check to see
		// if we should wrap to the new line
		if ( c > ' ' && ( x == 0 || text[y*LINE_WIDTH+x-1] <= ' ' ) ) {
			// count word length
			for (l=0 ; l< LINE_WIDTH ; l++) {
				if ( txt[l] <= ' ') {
					break;
				}
			}

			// word wrap
			if (l != LINE_WIDTH && (x + l >= LINE_WIDTH) ) {
				Linefeed();
			}
		}

		txt++;

		switch( c ) {
			case '\n':
				Linefeed ();
				break;
			case '\t':
				do {
					text[y*LINE_WIDTH+x] = (color << 8) | ' ';
					x++;
					if ( x >= LINE_WIDTH ) {
						Linefeed();
						x = 0;
					}
				} while ( x & 3 );
				break;
			case '\r':
				x = 0;
				break;
			default:	// display character and advance
				text[y*LINE_WIDTH+x] = (color << 8) | c;
				x++;
				if ( x >= LINE_WIDTH ) {
					Linefeed();
					x = 0;
				}
				break;
		}
	}


	// mark time for transparent overlay
	if ( current >= 0 ) {
		times[current % NUM_CON_TIMES] = Sys_Milliseconds();
	}
}


/*
==============================================================================

DRAWING

==============================================================================
*/


/*
================
DrawInput

Draw the editline after a ] prompt
================
*/
void idConsoleLocal::DrawInput() {
	int y, autoCompleteLength;

	y = vislines - ( SMALLCHAR_HEIGHT * 2 );

	if ( consoleField.GetAutoCompleteLength() != 0 ) {
		autoCompleteLength = strlen( consoleField.GetBuffer() ) - consoleField.GetAutoCompleteLength();

		if ( autoCompleteLength > 0 ) {
			renderSystem->DrawFilled( idVec4( 0.8f, 0.2f, 0.2f, 0.45f ),
				LOCALSAFE_LEFT + 2 * SMALLCHAR_WIDTH + consoleField.GetAutoCompleteLength() * SMALLCHAR_WIDTH,
				y + 2, autoCompleteLength * SMALLCHAR_WIDTH, SMALLCHAR_HEIGHT - 2 );
		}
	}

	renderSystem->SetColor( idStr::ColorForIndex( C_COLOR_CYAN ) );

	renderSystem->DrawSmallChar( LOCALSAFE_LEFT + 1 * SMALLCHAR_WIDTH, y, ']' );

	consoleField.Draw( LOCALSAFE_LEFT + 2 * SMALLCHAR_WIDTH, y, SCREEN_WIDTH - 3 * SMALLCHAR_WIDTH, true );
}


/*
================
DrawNotify

Draws the last few lines of output transparently over the game top
================
*/
void idConsoleLocal::DrawNotify() {
	int		x, v;
	short	*text_p;
	int		i;
	int		time;
	int		currentColor;

	if ( con_noPrint.GetBool() ) {
		return;
	}

	currentColor = idStr::ColorIndex( C_COLOR_WHITE );
	renderSystem->SetColor( idStr::ColorForIndex( currentColor ) );

	v = 0;
	for ( i = current-NUM_CON_TIMES+1; i <= current; i++ ) {
		if ( i < 0 ) {
			continue;
		}
		time = times[i % NUM_CON_TIMES];
		if ( time == 0 ) {
			continue;
		}
		time = Sys_Milliseconds() - time;
		if ( time > con_notifyTime.GetFloat() * 1000 ) {
			continue;
		}
		text_p = text + (i % TOTAL_LINES)*LINE_WIDTH;
		
		for ( x = 0; x < LINE_WIDTH; x++ ) {
			if ( ( text_p[x] & 0xff ) == ' ' ) {
				continue;
			}
			if ( idStr::ColorIndex(text_p[x]>>8) != currentColor ) {
				currentColor = idStr::ColorIndex(text_p[x]>>8);
				renderSystem->SetColor( idStr::ColorForIndex( currentColor ) );
			}
			renderSystem->DrawSmallChar( LOCALSAFE_LEFT + (x+1)*SMALLCHAR_WIDTH, v, text_p[x] & 0xff );
		}

		v += SMALLCHAR_HEIGHT;
	}

	renderSystem->SetColor( colorCyan );
}

/*
================
DrawSolidConsole

Draws the console with the solid background
================
*/
void idConsoleLocal::DrawSolidConsole( float frac ) {
	int				i, x;
	float			y;
	int				rows;
	short			*text_p;
	int				row;
	int				lines;
	int				currentColor;

	lines = idMath::Ftoi( SCREEN_HEIGHT * frac );
	if ( lines <= 0 ) {
		return;
	}

	if ( lines > SCREEN_HEIGHT ) {
		lines = SCREEN_HEIGHT;
	}

	// draw the background
	y = frac * SCREEN_HEIGHT - 2;
	if ( y < 1.0f ) {
		y = 0.0f;
	} else {
		renderSystem->DrawFilled( idVec4( 0.0f, 0.0f, 0.0f, 0.75f ), 0, 0, SCREEN_WIDTH, y );
	}

	renderSystem->DrawFilled( colorCyan, 0, y, SCREEN_WIDTH, 2 );

	// draw the version number

	renderSystem->SetColor( idStr::ColorForIndex( C_COLOR_CYAN ) );

	idStr version = va( "%s.%i.%i", ENGINE_VERSION, BUILD_NUMBER, BUILD_NUMBER_MINOR );
	i = version.Length();

	for ( x = 0; x < i; x++ ) {
		renderSystem->DrawSmallChar( LOCALSAFE_WIDTH - ( i - x ) * SMALLCHAR_WIDTH, 
			(lines-(SMALLCHAR_HEIGHT+SMALLCHAR_HEIGHT/4)), version[x] );

	}


	// draw the text
	vislines = lines;
	rows = (lines-SMALLCHAR_WIDTH)/SMALLCHAR_WIDTH;		// rows of text to draw

	y = lines - (SMALLCHAR_HEIGHT*3);

	// draw from the bottom up
	if ( display != current ) {
		// draw arrows to show the buffer is backscrolled
		renderSystem->SetColor( idStr::ColorForIndex( C_COLOR_CYAN ) );
		for ( x = 0; x < LINE_WIDTH; x += 4 ) {
			renderSystem->DrawSmallChar( LOCALSAFE_LEFT + (x+1)*SMALLCHAR_WIDTH, idMath::Ftoi( y ), '^' );
		}
		y -= SMALLCHAR_HEIGHT;
		rows--;
	}
	
	row = display;

	if ( x == 0 ) {
		row--;
	}

	currentColor = idStr::ColorIndex( C_COLOR_WHITE );
	renderSystem->SetColor( idStr::ColorForIndex( currentColor ) );

	for ( i = 0; i < rows; i++, y -= SMALLCHAR_HEIGHT, row-- ) {
		if ( row < 0 ) {
			break;
		}
		if ( current - row >= TOTAL_LINES ) {
			// past scrollback wrap point
			continue;	
		}

		text_p = text + (row % TOTAL_LINES)*LINE_WIDTH;

		for ( x = 0; x < LINE_WIDTH; x++ ) {
			if ( ( text_p[x] & 0xff ) == ' ' ) {
				continue;
			}

			if ( idStr::ColorIndex(text_p[x]>>8) != currentColor ) {
				currentColor = idStr::ColorIndex(text_p[x]>>8);
				renderSystem->SetColor( idStr::ColorForIndex( currentColor ) );
			}
			renderSystem->DrawSmallChar( LOCALSAFE_LEFT + (x+1)*SMALLCHAR_WIDTH, idMath::Ftoi( y ), text_p[x] & 0xff );
		}
	}

	// draw the input prompt, user text, and cursor if desired
	DrawInput();

	renderSystem->SetColor( colorCyan );
}


/*
==============
Draw

ForceFullScreen is used by the editor
==============
*/
void idConsoleLocal::Draw( bool forceFullScreen ) {
	if ( forceFullScreen ) {
		// if we are forced full screen because of a disconnect, 
		// we want the console closed when we go back to a session state
		Close();
		// we are however catching keyboard input
		keyCatching = true;
	}

	Scroll();

	UpdateDisplayFraction();

	if ( forceFullScreen ) {
		DrawSolidConsole( 1.0f );
	} else if ( displayFrac ) {
		DrawSolidConsole( displayFrac );
	} else {
		// only draw the notify lines if the developer cvar is set,
		// or we are a debug build
		if ( !con_noPrint.GetBool() ) {
			DrawNotify();
		}
	}

	float lefty = LOCALSAFE_TOP;
	float righty = LOCALSAFE_TOP;
	float centery = LOCALSAFE_TOP;
	if ( com_showFPS.GetBool() ) {
		righty = DrawFPS( righty );
	}
	if ( com_showMemoryUsage.GetBool() ) {
		righty = DrawMemoryUsage( righty );
	}
	DrawOverlayText( lefty, righty, centery );
	DrawDebugGraphs();
}

/*
========================
idConsoleLocal::PrintOverlay 
========================
*/
void idConsoleLocal::PrintOverlay( idOverlayHandle &handle, justify_t justify, const char *text, ... ) {
	if ( handle.index >= 0 && handle.index < overlayText.Num() ) {
		if ( overlayText[handle.index].time == handle.time ) {
			return;
		}
	}

	char string[MAX_PRINT_MSG];
	va_list argptr;
	va_start( argptr, text );
	idStr::vsnPrintf( string, sizeof( string ), text, argptr );
	va_end( argptr );

	overlayText_t &overlay = overlayText.Alloc();
	overlay.text = string;
	overlay.justify = justify;
	overlay.time = Sys_Milliseconds();

	handle.index = overlayText.Num() - 1;
	handle.time = overlay.time;
}

/*
========================
idConsoleLocal::DrawOverlayText
========================
*/
void idConsoleLocal::DrawOverlayText( float & leftY, float & rightY, float & centerY ) {
	for ( int i = 0; i < overlayText.Num(); i++ ) {
		const idStr & text = overlayText[i].text;

		int maxWidth = 0;
		int numLines = 0;
		for ( int j = 0; j < text.Length(); j++ ) {
			int width = 1;
			for (; j < text.Length() && text[j] != '\n'; j++ ) {
				width++;
			}
			numLines++;
			if ( width > maxWidth ) {
				maxWidth = width;
			}
		}

		idVec4 bgColor( 0.0f, 0.0f, 0.0f, 0.75f );

		const float width = maxWidth * SMALLCHAR_WIDTH;
		const float height = numLines * ( SMALLCHAR_HEIGHT + 4 );
		const float bgAdjust = - 0.5f * SMALLCHAR_WIDTH;
		if ( overlayText[i].justify == JUSTIFY_LEFT ) {
			renderSystem->DrawFilled( bgColor, LOCALSAFE_LEFT + bgAdjust, leftY, width, height );
		} else if ( overlayText[i].justify == JUSTIFY_RIGHT ) {
			renderSystem->DrawFilled( bgColor, LOCALSAFE_RIGHT - width + bgAdjust, rightY, width, height );
		} else if ( overlayText[i].justify == JUSTIFY_CENTER_LEFT || overlayText[i].justify == JUSTIFY_CENTER_RIGHT ) {
			renderSystem->DrawFilled( bgColor, LOCALSAFE_LEFT + ( LOCALSAFE_WIDTH - width + bgAdjust ) * 0.5f, centerY, width, height );
		} else {
			assert( false );
		}

		idStr singleLine;
		for ( int j = 0; j < text.Length(); j += singleLine.Length() + 1 ) {
			singleLine = "";
			for ( int k = j; k < text.Length() && text[k] != '\n'; k++ ) {
				singleLine.Append( text[k] );
			}
			if ( overlayText[i].justify == JUSTIFY_LEFT ) {
				DrawTextLeftAlign( LOCALSAFE_LEFT, leftY, "%s", singleLine.c_str() );
			} else if ( overlayText[i].justify == JUSTIFY_RIGHT ) {
				DrawTextRightAlign( LOCALSAFE_RIGHT, rightY, "%s", singleLine.c_str() );
			} else if ( overlayText[i].justify == JUSTIFY_CENTER_LEFT ) {
				DrawTextLeftAlign( LOCALSAFE_LEFT + ( LOCALSAFE_WIDTH - width ) * 0.5f, centerY, "%s", singleLine.c_str() );
			} else if ( overlayText[i].justify == JUSTIFY_CENTER_RIGHT ) {
				DrawTextRightAlign( LOCALSAFE_LEFT + ( LOCALSAFE_WIDTH + width ) * 0.5f, centerY, "%s", singleLine.c_str() );
			} else {
				assert( false );
			}
		}
	}
	overlayText.SetNum( 0 );
}

/*
========================
idConsoleLocal::CreateGraph
========================
*/
idDebugGraph * idConsoleLocal::CreateGraph( int numItems ) {
	idDebugGraph * graph = new (TAG_SYSTEM) idDebugGraph( numItems );
	debugGraphs.Append( graph );
	return graph;
}

/*
========================
idConsoleLocal::DestroyGraph
========================
*/
void idConsoleLocal::DestroyGraph( idDebugGraph * graph ) {
	debugGraphs.Remove( graph );
	delete graph;
}

/*
========================
idConsoleLocal::DrawDebugGraphs 
========================
*/
void idConsoleLocal::DrawDebugGraphs() {
	for ( int i = 0; i < debugGraphs.Num(); i++ ) {
		debugGraphs[i]->Render( renderSystem );
	}
}