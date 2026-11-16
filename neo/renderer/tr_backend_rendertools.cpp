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
#include "simplex.h"	// line font definition

/*
===============================================================================

	Debug tools.

	Nothing here draws on its own terms: every tool ends in RB_DrawElementsWithCounters, like the scene.

	  game, front end (game thread)                main thread, frame sync      back end (main thread)
	  -----------------------------                -----------------------      ----------------------
	  idRenderWorld::DebugLine, DrawText,   -+
	   DebugPolygon: the stores, kept until  |
	   the game clears them                  +->   RB_SnapshotDebugGeometry ->  RB_DrawDebugBatches,
	  R_AddViewDebugTools (portals, normals, |      idDrawVert + indexes in      once per 3D view
	   view entities, surface info): lines  -+      the frame's vertex cache
	   of one frame

	r_showTris, r_showLightCount, r_showLights, testImage and the line patterns are back-end passes.

===============================================================================
*/

idCVar r_showCenterOfProjection( "r_showCenterOfProjection", "0", CVAR_RENDERER | CVAR_BOOL, "Draw a cross to show the center of projection" );
idCVar r_showLines( "r_showLines", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = draw alternate horizontal lines, 2 = draw alternate vertical lines" );

#define MAX_DEBUG_LINES			16384

typedef struct debugLine_s {
	idVec4		rgb;
	idVec3		start;
	idVec3		end;
	bool		depthTest;
	int			lifeTime;
} debugLine_t;

debugLine_t		rb_debugLines[ MAX_DEBUG_LINES ];
int				rb_numDebugLines = 0;
int				rb_debugLineTime = 0;

#define MAX_DEBUG_TEXT			512

typedef struct debugText_s {
	idStr		text;
	idVec3		origin;
	float		scale;
	idVec4		color;
	idMat3		viewAxis;
	int			align;
	int			lifeTime;
	bool		depthTest;
} debugText_t;

debugText_t		rb_debugText[ MAX_DEBUG_TEXT ];
int				rb_numDebugText = 0;
int				rb_debugTextTime = 0;

#define MAX_DEBUG_POLYGONS		8192

typedef struct debugPolygon_s {
	idVec4		rgb;
	idWinding	winding;
	bool		depthTest;
	int			lifeTime;
} debugPolygon_t;

debugPolygon_t	rb_debugPolygons[ MAX_DEBUG_POLYGONS ];
int				rb_numDebugPolygons = 0;
int				rb_debugPolygonTime = 0;

// lines the front end adds for the frame it builds; the snapshot draws them once and empties the list
static idList<debugLine_t>	rb_frameDebugLines( 4096 );

void RB_SetMVP( const idRenderMatrix & mvp );

/*
================
RB_ResetViewScissor
================
*/
static void RB_ResetViewScissor() {
	renderBackend->Scissor( backEnd.viewDef->viewport.x1 + backEnd.viewDef->scissor.x1,
				backEnd.viewDef->viewport.y1 + backEnd.viewDef->scissor.y1,
				backEnd.viewDef->scissor.x2 + 1 - backEnd.viewDef->scissor.x1,
				backEnd.viewDef->scissor.y2 + 1 - backEnd.viewDef->scissor.y1 );
	backEnd.currentScissor = backEnd.viewDef->scissor;
}

/*
================
RB_SimpleSurfaceSetup
================
*/
static void RB_SimpleSurfaceSetup( const drawSurf_t *drawSurf ) {
	// change the matrix if needed
	if ( drawSurf->space != backEnd.currentSpace ) {
		RB_SetMVP( drawSurf->space->mvp );
		backEnd.currentSpace = drawSurf->space;
	}

	// change the scissor if needed
	if ( !backEnd.currentScissor.Equals( drawSurf->scissorRect ) && r_useScissor.GetBool() ) {
		renderBackend->Scissor( backEnd.viewDef->viewport.x1 + drawSurf->scissorRect.x1,
					backEnd.viewDef->viewport.y1 + drawSurf->scissorRect.y1,
					drawSurf->scissorRect.x2 + 1 - drawSurf->scissorRect.x1,
					drawSurf->scissorRect.y2 + 1 - drawSurf->scissorRect.y1 );
		backEnd.currentScissor = drawSurf->scissorRect;
	}
}

/*
================
RB_SetTextureColorParms

The parms of the texture_color programs: the white image on slot 0, no texgen, no alpha test.
The colour is the vertex colour, or the one of RB_SetColor.
================
*/
static void RB_SetTextureColorParms( bool useVertexColor ) {
	static const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	static const float one[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	static const float texS[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
	static const float texT[4] = { 0.0f, 1.0f, 0.0f, 0.0f };

	globalImages->whiteImage->Bind( 0 );

	renderProgManager.SetRenderParm( RENDERPARM_TEXTUREMATRIX_S, texS );
	renderProgManager.SetRenderParm( RENDERPARM_TEXTUREMATRIX_T, texT );
	renderProgManager.SetRenderParm( RENDERPARM_TEXGEN_0_ENABLED, zero );
	renderProgManager.SetRenderParm( RENDERPARM_ALPHA_TEST, zero );
	renderProgManager.SetRenderParm( RENDERPARM_VERTEXCOLOR_MODULATE, useVertexColor ? one : zero );
	renderProgManager.SetRenderParm( RENDERPARM_VERTEXCOLOR_ADD, useVertexColor ? zero : one );
}

/*
================
RB_DrawSurfaceOneColor

There is no skinned colour-only program, so the texture_color pair draws with the white image.
================
*/
static void RB_DrawSurfaceOneColor( const drawSurf_t * surf ) {
	if ( surf->jointCache ) {
		renderProgManager.BindShader_TextureVertexColorSkinned();
	} else {
		renderProgManager.BindShader_TextureVertexColor();
	}
	RB_DrawElementsWithCounters( surf );
}

/*
================
RB_ClearRect

A scissored clear as a drawing primitive, in window pixels. The caller restores the scissor.
================
*/
static void RB_ClearRect( int x, int y, int width, int height, const idVec4 & color ) {
	if ( x < 0 ) {
		width += x;
		x = 0;
	}
	if ( y < 0 ) {
		height += y;
		y = 0;
	}
	if ( width <= 0 || height <= 0 ) {
		return;
	}
	renderBackend->Scissor( x, y, width, height );
	renderBackend->Clear( true, false, false, 0, color.x, color.y, color.z, color.w );
}

/*
================
RB_RestoreScissor

Puts back the scissor backEnd.currentScissor stands for, after RB_ClearRect.
================
*/
static void RB_RestoreScissor() {
	renderBackend->Scissor( backEnd.viewDef->viewport.x1 + backEnd.currentScissor.x1,
				backEnd.viewDef->viewport.y1 + backEnd.currentScissor.y1,
				backEnd.currentScissor.x2 + 1 - backEnd.currentScissor.x1,
				backEnd.currentScissor.y2 + 1 - backEnd.currentScissor.y1 );
}

/*
===================
R_ColorByStencilBuffer

Sets the screen colors based on the contents of the
stencil buffer.  Stencil of 0 = black, 1 = red, 2 = green,
3 = blue, ..., 7+ = white
===================
*/
static void R_ColorByStencilBuffer() {
	static const float colors[7][3] = {
		{0,0,0},
		{1,0,0},
		{0,1,0},
		{0,0,1},
		{0,1,1},
		{1,0,1},
		{1,1,0},
	};

	// clear color buffer to white (more passes than the table has)
	RB_ResetViewScissor();
	renderBackend->State( GLS_DEPTHMASK | GLS_DEPTHFUNC_ALWAYS );
	renderBackend->Clear( true, false, false, 0, 1.0f, 1.0f, 1.0f, 1.0f );

	// now draw color for each stencil value
	renderBackend->Cull( CT_TWO_SIDED );
	RB_SetMVP( renderMatrix_identity );
	backEnd.currentSpace = NULL;
	renderProgManager.BindShader_Color();
	for ( int i = 0; i < 7; i++ ) {
		renderBackend->State( GLS_DEPTHMASK | GLS_DEPTHFUNC_ALWAYS | GLS_STENCIL_FUNC_EQUAL | GLS_STENCIL_MAKE_REF( i ) | GLS_STENCIL_MAKE_MASK( 255 ) );
		RB_SetColor( colors[i][0], colors[i][1], colors[i][2] );
		RB_DrawElementsWithCounters( &backEnd.unitSquareSurface );
	}

	renderBackend->State( GLS_DEFAULT );
}

//======================================================================

/*
=================
RB_ShowLightCount

This is a debugging tool that will draw each surface with a color
based on how many lights are effecting it
=================
*/
static void RB_ShowLightCount() {
	int		i;
	const drawSurf_t	*surf;
	const viewLight_t	*vLight;

	if ( !r_showLightCount.GetBool() ) {
		return;
	}

	RB_ResetViewScissor();

	renderBackend->Clear( false, false, true, 0, 0.0f, 0.0f, 0.0f, 0.0f );

	// only the stencil buffer is written; optionally count everything through walls
	const uint64 countBits = GLS_COLORMASK | GLS_ALPHAMASK | GLS_DEPTHMASK | GLS_DEPTHFUNC_EQUAL | GLS_STENCIL_MAKE_MASK( 255 );
	if ( r_showLightCount.GetInteger() >= 2 ) {
		renderBackend->State( countBits | GLS_STENCIL_OP_FAIL_KEEP | GLS_STENCIL_OP_ZFAIL_INCR | GLS_STENCIL_OP_PASS_INCR );
	} else {
		renderBackend->State( countBits | GLS_STENCIL_OP_FAIL_KEEP | GLS_STENCIL_OP_ZFAIL_KEEP | GLS_STENCIL_OP_PASS_INCR );
	}
	renderBackend->Cull( CT_FRONT_SIDED );

	RB_SetColor( 1.0f, 1.0f, 1.0f );
	RB_SetTextureColorParms( false );

	backEnd.currentSpace = NULL;

	for ( vLight = backEnd.viewDef->viewLights; vLight; vLight = vLight->next ) {
		for ( i = 0; i < 2; i++ ) {
			for ( surf = i ? vLight->localInteractions: vLight->globalInteractions; surf; surf = (drawSurf_t *)surf->nextOnLight ) {
				RB_SimpleSurfaceSetup( surf );
				RB_DrawSurfaceOneColor( surf );
			}
		}
	}

	// display the results
	R_ColorByStencilBuffer();
}

/*
=====================
RB_ShowTris

Debugging tool
=====================
*/
static void RB_ShowTris( drawSurf_t **drawSurfs, int numDrawSurfs ) {
	if ( r_showTris.GetInteger() == 0 ) {
		return;
	}

	float color[4] = { 1, 1, 1, 1 };

	renderBackend->PolygonOffset( -1.0f, -2.0f );

	switch ( r_showTris.GetInteger() ) {
		case 1: // only draw visible ones
			renderBackend->State( GLS_DEPTHMASK | GLS_ALPHAMASK | GLS_POLYMODE_LINE | GLS_POLYGON_OFFSET );
			break;
		case 2:	// draw all front facing
		case 3: // draw all
			renderBackend->State( GLS_DEPTHMASK | GLS_ALPHAMASK | GLS_POLYMODE_LINE | GLS_POLYGON_OFFSET | GLS_DEPTHFUNC_ALWAYS );
			break;
		case 4: // only draw visible ones with blended lines
			renderBackend->State( GLS_DEPTHMASK | GLS_ALPHAMASK | GLS_POLYMODE_LINE | GLS_POLYGON_OFFSET | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA );
			color[3] = 0.4f;
			break;
	}

	renderBackend->Cull( ( r_showTris.GetInteger() == 3 ) ? CT_TWO_SIDED : CT_FRONT_SIDED );

	RB_SetColor( color );
	RB_SetTextureColorParms( false );

	backEnd.currentSpace = NULL;

	for ( int i = 0 ; i < numDrawSurfs ; i++ ) {
		const drawSurf_t * drawSurf = drawSurfs[i];
		if ( drawSurf == NULL || drawSurf->space == NULL ) {
			continue;
		}
		RB_SimpleSurfaceSetup( drawSurf );
		RB_DrawSurfaceOneColor( drawSurf );
	}

	if ( r_showTris.GetInteger() == 3 ) {
		renderBackend->Cull( CT_FRONT_SIDED );
	}
}

/*
==============
RB_ShowLights

Visualize all light volumes used in the current scene
r_showLights 1	: just print volumes numbers, highlighting ones covering the view
r_showLights 2	: also draw planes of each volume
r_showLights 3	: also draw edges of each volume
==============
*/
static void RB_ShowLights() {
	if ( !r_showLights.GetInteger() ) {
		return;
	}

	renderBackend->State( GLS_DEFAULT );
	RB_ResetViewScissor();

	// the volumes are drawn with 'vLight->inverseBaseLightProject'
	backEnd.currentSpace = NULL;

	renderProgManager.BindShader_Color();

	renderBackend->Cull( CT_TWO_SIDED );

	common->Printf( "volumes: " );	// FIXME: not in back end!

	int count = 0;
	for ( viewLight_t * vLight = backEnd.viewDef->viewLights; vLight != NULL; vLight = vLight->next ) {
		count++;

		// depth buffered planes
		if ( r_showLights.GetInteger() >= 2 ) {
			renderBackend->State( GLS_DEPTHFUNC_ALWAYS | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHMASK );
			RB_SetColor( 0.0f, 0.0f, 1.0f, 0.25f );
			idRenderMatrix invProjectMVPMatrix;
			idRenderMatrix::Multiply( backEnd.viewDef->worldSpace.mvp, vLight->inverseBaseLightProject, invProjectMVPMatrix );
			RB_SetMVP( invProjectMVPMatrix );
			RB_DrawElementsWithCounters( &backEnd.zeroOneCubeSurface );
		}

		// non-hidden lines
		if ( r_showLights.GetInteger() >= 3 ) {
			renderBackend->State( GLS_DEPTHFUNC_ALWAYS | GLS_POLYMODE_LINE | GLS_DEPTHMASK  );
			RB_SetColor( 1.0f, 1.0f, 1.0f );
			idRenderMatrix invProjectMVPMatrix;
			idRenderMatrix::Multiply( backEnd.viewDef->worldSpace.mvp, vLight->inverseBaseLightProject, invProjectMVPMatrix );
			RB_SetMVP( invProjectMVPMatrix );
			RB_DrawElementsWithCounters( &backEnd.zeroOneCubeSurface );
		}

		common->Printf( "%i ", vLight->lightDef->index );
	}

	common->Printf( " = %i total\n", count );
}

/*
================
RB_ClearDebugText
================
*/
void RB_ClearDebugText( int time ) {
	int			i;
	int			num;
	debugText_t	*text;

	rb_debugTextTime = time;

	if ( !time ) {
		// free up our strings
		text = rb_debugText;
		for ( i = 0; i < MAX_DEBUG_TEXT; i++, text++ ) {
			text->text.Clear();
		}
		rb_numDebugText = 0;
		return;
	}

	// copy any text that still needs to be drawn
	num	= 0;
	text = rb_debugText;
	for ( i = 0; i < rb_numDebugText; i++, text++ ) {
		if ( text->lifeTime > time ) {
			if ( num != i ) {
				rb_debugText[ num ] = *text;
			}
			num++;
		}
	}
	rb_numDebugText = num;
}

/*
================
RB_AddDebugText
================
*/
void RB_AddDebugText( const char *text, const idVec3 &origin, float scale, const idVec4 &color, const idMat3 &viewAxis, const int align, const int lifetime, const bool depthTest ) {
	debugText_t *debugText;

	if ( rb_numDebugText < MAX_DEBUG_TEXT ) {
		debugText = &rb_debugText[ rb_numDebugText++ ];
		debugText->text			= text;
		debugText->origin		= origin;
		debugText->scale		= scale;
		debugText->color		= color;
		debugText->viewAxis		= viewAxis;
		debugText->align		= align;
		debugText->lifeTime		= rb_debugTextTime + lifetime;
		debugText->depthTest	= depthTest;
	}
}

/*
================
RB_DrawTextLength

  returns the length of the given text
================
*/
float RB_DrawTextLength( const char *text, float scale, int len ) {
	int i, num, index, charIndex;
	float spacing, textLen = 0.0f;

	if ( text && *text ) {
		if ( !len ) {
			len = idStr::Length(text);
		}
		for ( i = 0; i < len; i++ ) {
			charIndex = text[i] - 32;
			if ( charIndex < 0 || charIndex > NUM_SIMPLEX_CHARS ) {
				continue;
			}
			num = simplex[charIndex][0] * 2;
			spacing = simplex[charIndex][1];
			index = 2;

			while( index - 2 < num ) {
				if ( simplex[charIndex][index] < 0) {
					index++;
					continue;
				}
				index += 2;
				if ( simplex[charIndex][index] < 0) {
					index++;
					continue;
				}
			}
			textLen += spacing * scale;
		}
	}
	return textLen;
}

/*
================
RB_TextStrokes

  Calls stroke( p1, p2 ) for every line of the text in the simplex font.
  oriented on the viewaxis
  align can be 0-left, 1-center (default), 2-right
================
*/
template< typename _stroke_ >
static void RB_TextStrokes( const char *text, const idVec3 &origin, float scale, const idMat3 &viewAxis, const int align, _stroke_ && stroke ) {
	int i, j, len, num, index, charIndex, line;
	float textLen = 1.0f, spacing = 1.0f;
	idVec3 org, p1, p2;

	if ( text && *text ) {
		if ( text[0] == '\n' ) {
			line = 1;
		} else {
			line = 0;
		}

		len = idStr::Length( text );
		for ( i = 0; i < len; i++ ) {

			if ( i == 0 || text[i] == '\n' ) {
				org = origin - viewAxis[2] * ( line * 36.0f * scale );
				if ( align != 0 ) {
					for ( j = 1; i+j <= len; j++ ) {
						if ( i+j == len || text[i+j] == '\n' ) {
							textLen = RB_DrawTextLength( text+i, scale, j );
							break;
						}
					}
					if ( align == 2 ) {
						// right
						org += viewAxis[1] * textLen;
					} else {
						// center
						org += viewAxis[1] * ( textLen * 0.5f );
					}
				}
				line++;
			}

			charIndex = text[i] - 32;
			if ( charIndex < 0 || charIndex > NUM_SIMPLEX_CHARS ) {
				continue;
			}
			num = simplex[charIndex][0] * 2;
			spacing = simplex[charIndex][1];
			index = 2;

			while( index - 2 < num ) {
				if ( simplex[charIndex][index] < 0) {
					index++;
					continue;
				}
				p1 = org + scale * simplex[charIndex][index] * -viewAxis[1] + scale * simplex[charIndex][index+1] * viewAxis[2];
				index += 2;
				if ( simplex[charIndex][index] < 0) {
					index++;
					continue;
				}
				p2 = org + scale * simplex[charIndex][index] * -viewAxis[1] + scale * simplex[charIndex][index+1] * viewAxis[2];

				stroke( p1, p2 );
			}
			org -= viewAxis[1] * ( spacing * scale );
		}
	}
}

/*
================
RB_ClearDebugLines
================
*/
void RB_ClearDebugLines( int time ) {
	int			i;
	int			num;
	debugLine_t	*line;

	rb_debugLineTime = time;

	if ( !time ) {
		rb_numDebugLines = 0;
		return;
	}

	// copy any lines that still need to be drawn
	num	= 0;
	line = rb_debugLines;
	for ( i = 0; i < rb_numDebugLines; i++, line++ ) {
		if ( line->lifeTime > time ) {
			if ( num != i ) {
				rb_debugLines[ num ] = *line;
			}
			num++;
		}
	}
	rb_numDebugLines = num;
}

/*
================
RB_AddDebugLine
================
*/
void RB_AddDebugLine( const idVec4 &color, const idVec3 &start, const idVec3 &end, const int lifeTime, const bool depthTest ) {
	debugLine_t *line;

	if ( rb_numDebugLines < MAX_DEBUG_LINES ) {
		line = &rb_debugLines[ rb_numDebugLines++ ];
		line->rgb		= color;
		line->start		= start;
		line->end		= end;
		line->depthTest = depthTest;
		line->lifeTime	= rb_debugLineTime + lifeTime;
	}
}

/*
================
RB_ClearDebugPolygons
================
*/
void RB_ClearDebugPolygons( int time ) {
	int				i;
	int				num;
	debugPolygon_t	*poly;

	rb_debugPolygonTime = time;

	if ( !time ) {
		rb_numDebugPolygons = 0;
		return;
	}

	// copy any polygons that still need to be drawn
	num	= 0;

	poly = rb_debugPolygons;
	for ( i = 0; i < rb_numDebugPolygons; i++, poly++ ) {
		if ( poly->lifeTime > time ) {
			if ( num != i ) {
				rb_debugPolygons[ num ] = *poly;
			}
			num++;
		}
	}
	rb_numDebugPolygons = num;
}

/*
================
RB_AddDebugPolygon
================
*/
void RB_AddDebugPolygon( const idVec4 &color, const idWinding &winding, const int lifeTime, const bool depthTest ) {
	debugPolygon_t *poly;

	if ( rb_numDebugPolygons < MAX_DEBUG_POLYGONS ) {
		poly = &rb_debugPolygons[ rb_numDebugPolygons++ ];
		poly->rgb		= color;
		poly->winding	= winding;
		poly->depthTest = depthTest;
		poly->lifeTime	= rb_debugPolygonTime + lifeTime;
	}
}

/*
===============================================================================

	Lines of one frame, added by the front end (game thread, outside jobs)

===============================================================================
*/

/*
================
R_AddFrameDebugLine

Drawn on top of the scene unless r_debugLineDepthTest is set, like a debug line without depth test.
================
*/
void R_AddFrameDebugLine( const idVec4 & color, const idVec3 & start, const idVec3 & end ) {
	debugLine_t & line = rb_frameDebugLines.Alloc();
	line.rgb = color;
	line.start = start;
	line.end = end;
	line.depthTest = false;
	line.lifeTime = 0;
}

/*
================
R_AddFrameDebugText
================
*/
void R_AddFrameDebugText( const char * text, const idVec3 & origin, float scale, const idVec4 & color, const idMat3 & viewAxis, const int align ) {
	RB_TextStrokes( text, origin, scale, viewAxis, align, [&color]( const idVec3 & p1, const idVec3 & p2 ) {
		R_AddFrameDebugLine( color, p1, p2 );
	} );
}

/*
================
R_AddFrameDebugBounds

The edges of a box in the space of modelMatrix.
================
*/
static void R_AddFrameDebugBounds( const idVec4 & color, const idBounds & bounds, const float modelMatrix[16] ) {
	if ( bounds.IsCleared() ) {
		return;
	}

	idVec3 v[8];
	for ( int i = 0; i < 8; i++ ) {
		const idVec3 local( bounds[(i^(i>>1))&1][0], bounds[(i>>1)&1][1], bounds[(i>>2)&1][2] );
		R_LocalPointToGlobal( modelMatrix, local, v[i] );
	}
	for ( int i = 0; i < 4; i++ ) {
		R_AddFrameDebugLine( color, v[i], v[(i+1)&3] );
		R_AddFrameDebugLine( color, v[4+i], v[4+((i+1)&3)] );
		R_AddFrameDebugLine( color, v[i], v[4+i] );
	}
}

/*
=====================
R_ShowSurfaceInfo

Debugging tool
=====================
*/
static void R_ShowSurfaceInfo( const viewDef_t * viewDef ) {
	if ( !r_showSurfaceInfo.GetBool() ) {
		return;
	}

	const renderView_t & renderView = viewDef->renderView;

	// start far enough away that we don't hit the player model
	modelTrace_t mt;
	const idVec3 start = renderView.vieworg + renderView.viewaxis[0] * 16;
	const idVec3 end = start + renderView.viewaxis[0] * 1000.0f;
	if ( !viewDef->renderWorld->Trace( mt, start, end, 0.0f, false ) ) {
		return;
	}

	if ( mt.entity != NULL && mt.entity->hModel != NULL ) {
		R_AddFrameDebugText( mt.entity->hModel->Name(), mt.point + renderView.viewaxis[2] * 12,
			0.35f, colorRed, renderView.viewaxis, 1 );
	}
	if ( mt.material != NULL ) {
		R_AddFrameDebugText( mt.material->GetName(), mt.point,
			0.35f, colorBlue, renderView.viewaxis, 1 );
	}
}

/*
=====================
R_ShowViewEntitys

Debugging tool
=====================
*/
static void R_ShowViewEntitys( const viewDef_t * viewDef ) {
	if ( !r_showViewEntitys.GetBool() ) {
		return;
	}
	if ( r_showViewEntitys.GetInteger() >= 2 ) {
		common->Printf( "view entities: " );
		for ( const viewEntity_t * vModel = viewDef->viewEntitys; vModel; vModel = vModel->next ) {
			if ( vModel->entityDef == NULL ) {
				continue;	// the space of a gui
			}
			if ( vModel->entityDef->IsDirectlyVisible() ) {
				common->Printf( "<%i> ", vModel->entityDef->index );
			} else {
				common->Printf( "%i ", vModel->entityDef->index );
			}
		}
		common->Printf( "\n" );
	}

	for ( const viewEntity_t * vModel = viewDef->viewEntitys; vModel; vModel = vModel->next ) {
		idRenderEntityLocal * edef = vModel->entityDef;
		if ( !edef ) {
			continue;
		}

		// draw the model bounds in white if directly visible,
		// or, blue if it is only-for-sahdow
		const idVec4 & color = edef->IsDirectlyVisible() ? colorWhite : colorBlue;
		R_AddFrameDebugBounds( color, edef->localReferenceBounds, vModel->modelMatrix );

		// transform the upper bounds corner into global space
		if ( r_showViewEntitys.GetInteger() >= 2 ) {
			idVec3 corner;
			R_LocalPointToGlobal( vModel->modelMatrix, edef->localReferenceBounds[1], corner );

			// a def drawn through its callback has no model of its own
			R_AddFrameDebugText(
				va( "%i:%s", edef->index, ( edef->parms.hModel != NULL ) ? edef->parms.hModel->Name() : "<callback>" ),
				corner,
				0.25f, color,
				viewDef->renderView.viewaxis, 1 );
		}

		// draw the actual bounds in yellow if different
		if ( r_showViewEntitys.GetInteger() >= 3 ) {
			idRenderModel *model = R_EntityDefDynamicModel( edef );
			if ( !model ) {
				continue;	// particles won't instantiate without a current view
			}
			const idBounds b = model->Bounds( &edef->parms );
			if ( b != edef->localReferenceBounds ) {
				R_AddFrameDebugBounds( colorYellow, b, vModel->modelMatrix );
			}
		}
	}
}

/*
=====================
R_ShowNormals

Debugging tool
=====================
*/
static void R_ShowNormals( const viewDef_t * viewDef ) {
	if ( r_showNormals.GetFloat() == 0.0f ) {
		return;
	}

	float size = r_showNormals.GetFloat();
	bool showNumbers = false;
	if ( size < 0.0f ) {
		size = -size;
		showNumbers = true;
	}

	for ( int i = 0; i < viewDef->numDrawSurfs; i++ ) {
		const drawSurf_t * drawSurf = viewDef->drawSurfs[i];
		const srfTriangles_t * tri = drawSurf->frontEndGeo;
		if ( tri == NULL || tri->verts == NULL ) {
			continue;
		}
		const float * modelMatrix = drawSurf->space->modelMatrix;

		for ( int j = 0; j < tri->numVerts; j++ ) {
			const idVec3 & xyz = tri->verts[j].xyz;
			const idVec3 normal = tri->verts[j].GetNormal();
			const idVec3 tangent = tri->verts[j].GetTangent();
			const idVec3 bitangent = tri->verts[j].GetBiTangent();

			idVec3 start, end;
			R_LocalPointToGlobal( modelMatrix, xyz, start );

			R_LocalPointToGlobal( modelMatrix, xyz + size * normal, end );
			R_AddFrameDebugLine( colorBlue, start, end );

			R_LocalPointToGlobal( modelMatrix, xyz + size * tangent, end );
			R_AddFrameDebugLine( colorRed, start, end );

			R_LocalPointToGlobal( modelMatrix, xyz + size * bitangent, end );
			R_AddFrameDebugLine( colorGreen, start, end );
		}

		if ( showNumbers ) {
			idVec3 pos;
			for ( int j = 0; j < tri->numVerts; j++ ) {
				const idVec3 normal = tri->verts[j].GetNormal();
				const idVec3 tangent = tri->verts[j].GetTangent();
				R_LocalPointToGlobal( modelMatrix, tri->verts[j].xyz + tangent + normal * 0.2f, pos );
				R_AddFrameDebugText( va( "%d", j ), pos, 0.01f, colorWhite, viewDef->renderView.viewaxis, 1 );
			}

			for ( int j = 0; j < tri->numIndexes; j += 3 ) {
				const idVec3 normal = tri->verts[ tri->indexes[ j + 0 ] ].GetNormal();
				R_LocalPointToGlobal( modelMatrix, ( tri->verts[ tri->indexes[ j + 0 ] ].xyz + tri->verts[ tri->indexes[ j + 1 ] ].xyz + tri->verts[ tri->indexes[ j + 2 ] ].xyz ) * ( 1.0f / 3.0f ) + normal * 0.2f, pos );
				R_AddFrameDebugText( va( "%d", j / 3 ), pos, 0.01f, colorCyan, viewDef->renderView.viewaxis, 1 );
			}
		}
	}
}

/*
=====================
R_AddViewDebugTools

The tools that only add lines and text. R_RenderView calls it for the main view, after the
draw surfaces are sorted and before a subview changes tr.viewCount.
=====================
*/
void R_AddViewDebugTools( const viewDef_t * viewDef ) {
	if ( viewDef->isSubview ) {
		return;
	}
	if ( r_showPortals.GetBool() ) {
		static_cast<idRenderWorldLocal *>( viewDef->renderWorld )->ShowPortals();
	}
	R_ShowSurfaceInfo( viewDef );
	R_ShowNormals( viewDef );
	R_ShowViewEntitys( viewDef );
}

/*
===============================================================================

	The snapshot: stores -> vertex cache -> backEnd.debugBatches

===============================================================================
*/

// indexes are 16 bit, so a batch ends at this many vertices
static const int MAX_DEBUG_BATCH_VERTS = 65536;
static const int INDEXES_PER_CACHE_BLOCK = INDEX_CACHE_ALIGN / static_cast<int>( sizeof( triIndex_t ) );
static_assert( sizeof( idDrawVert ) % VERTEX_CACHE_ALIGN == 0 );

class idDebugBatchBuilder {
public:
	void	Begin( primitive_t topology, uint64 stateBits );
	void	Line( const idVec3 & start, const idVec3 & end, dword color );
	void	Polygon( const idWinding & winding, dword color );
	void	Flush();
	void	FreeMemory() { verts.Clear(); indexes.Clear(); }

private:
	int		AddVert( const idVec3 & xyz, dword color );

	idList<idDrawVert>	verts { 8192 };
	idList<triIndex_t>	indexes { 8192 };
	primitive_t			topology = primitive_t::LINES;
	uint64				stateBits = 0;
};

static idDebugBatchBuilder rb_debugBatchBuilder;

/*
================
idDebugBatchBuilder::Begin

Geometry with the same topology and state as the batch before joins it.
================
*/
void idDebugBatchBuilder::Begin( primitive_t topology_, uint64 stateBits_ ) {
	if ( topology != topology_ || stateBits != stateBits_ ) {
		Flush();
		topology = topology_;
		stateBits = stateBits_;
	}
}

/*
================
idDebugBatchBuilder::AddVert
================
*/
int idDebugBatchBuilder::AddVert( const idVec3 & xyz, dword color ) {
	idDrawVert & vert = verts.Alloc();
	vert.Clear();
	vert.xyz = xyz;
	vert.SetColor( color );
	return verts.Num() - 1;
}

/*
================
idDebugBatchBuilder::Line
================
*/
void idDebugBatchBuilder::Line( const idVec3 & start, const idVec3 & end, dword color ) {
	if ( verts.Num() + 2 > MAX_DEBUG_BATCH_VERTS ) {
		Flush();
	}
	indexes.Append( static_cast<triIndex_t>( AddVert( start, color ) ) );
	indexes.Append( static_cast<triIndex_t>( AddVert( end, color ) ) );
}

/*
================
idDebugBatchBuilder::Polygon

A triangle fan, or the outline when the batch is made of lines.
================
*/
void idDebugBatchBuilder::Polygon( const idWinding & winding, dword color ) {
	const int numPoints = winding.GetNumPoints();
	if ( topology == primitive_t::LINES ) {
		for ( int i = 0; i < numPoints && numPoints >= 2; i++ ) {
			Line( winding[i].ToVec3(), winding[( i + 1 ) % numPoints].ToVec3(), color );
		}
		return;
	}
	if ( numPoints < 3 || numPoints > MAX_DEBUG_BATCH_VERTS ) {
		return;
	}
	if ( verts.Num() + numPoints > MAX_DEBUG_BATCH_VERTS ) {
		Flush();
	}
	const int first = verts.Num();
	for ( int i = 0; i < numPoints; i++ ) {
		AddVert( winding[i].ToVec3(), color );
	}
	for ( int i = 2; i < numPoints; i++ ) {
		indexes.Append( static_cast<triIndex_t>( first ) );
		indexes.Append( static_cast<triIndex_t>( first + i - 1 ) );
		indexes.Append( static_cast<triIndex_t>( first + i ) );
	}
}

/*
================
idDebugBatchBuilder::Flush

Copies the batch into the frame set of the vertex cache. A batch that does not fit any more is
dropped: running out of the cache is an error for the scene, not for a debug tool.
================
*/
void idDebugBatchBuilder::Flush() {
	const int numIndexes = indexes.Num();
	if ( numIndexes > 0 && backEnd.numDebugBatches < MAX_DEBUG_BATCHES ) {
		// the cache copies whole 16 byte blocks
		while ( ( indexes.Num() % INDEXES_PER_CACHE_BLOCK ) != 0 ) {
			indexes.Append( 0 );
		}
		const int vertBytes = verts.Num() * static_cast<int>( sizeof( idDrawVert ) );
		const int indexBytes = indexes.Num() * static_cast<int>( sizeof( triIndex_t ) );

		geoBufferSet_t & frameSet = vertexCache.frameData[vertexCache.listNum];
		if ( frameSet.vertexMemUsed.GetValue() + vertBytes <= frameSet.vertexBuffer.GetSize() &&
				frameSet.indexMemUsed.GetValue() + indexBytes <= frameSet.indexBuffer.GetSize() ) {
			debugBatch_t & batch = backEnd.debugBatches[backEnd.numDebugBatches++];
			batch.vertexCache = vertexCache.AllocVertex( verts.Ptr(), vertBytes );
			batch.indexCache = vertexCache.AllocIndex( indexes.Ptr(), indexBytes );
			batch.numIndexes = numIndexes;
			batch.topology = topology;
			batch.stateBits = stateBits;
		}
	}
	verts.SetNum( 0 );
	indexes.SetNum( 0 );
}

/*
================
RB_SnapshotDebugGeometry

Main thread, at the frame sync point before vertexCache.BeginBackEnd(): the game thread is parked,
so the stores are whole. With empty stores this is four compares.
================
*/
void RB_SnapshotDebugGeometry() {
	backEnd.numDebugBatches = 0;

	if ( rb_numDebugLines == 0 && rb_numDebugText == 0 && rb_numDebugPolygons == 0 && rb_frameDebugLines.Num() == 0 ) {
		return;
	}

	idDebugBatchBuilder & builder = rb_debugBatchBuilder;

	// lines are drawn as lines, so GLS_POLYMODE_LINE is not part of their state
	const uint64 onTopBits = r_debugLineDepthTest.GetBool() ? GLS_DEFAULT : GLS_DEPTHFUNC_ALWAYS;
	const uint64 depthTestBits = GLS_DEFAULT;

	// lines without the depth test are opaque whatever their alpha says
	const auto opaque = []( const idVec4 & rgb ) { return PackColor( idVec4( rgb.x, rgb.y, rgb.z, 1.0f ) ); };

	for ( int pass = 0; pass < 2; pass++ ) {
		const bool depthTest = ( pass == 1 );
		builder.Begin( primitive_t::LINES, depthTest ? depthTestBits : onTopBits );

		const debugLine_t * line = rb_debugLines;
		for ( int i = 0; i < rb_numDebugLines; i++, line++ ) {
			if ( line->depthTest == depthTest ) {
				builder.Line( line->start, line->end, depthTest ? PackColor( line->rgb ) : opaque( line->rgb ) );
			}
		}
		if ( !depthTest ) {
			for ( int i = 0; i < rb_frameDebugLines.Num(); i++ ) {
				const debugLine_t & frameLine = rb_frameDebugLines[i];
				builder.Line( frameLine.start, frameLine.end, opaque( frameLine.rgb ) );
			}
		}
	}
	rb_frameDebugLines.SetNum( 0 );

	for ( int pass = 0; pass < 2; pass++ ) {
		const bool depthTest = ( pass == 1 );
		builder.Begin( primitive_t::LINES, depthTest ? depthTestBits : onTopBits );

		const debugText_t * text = rb_debugText;
		for ( int i = 0; i < rb_numDebugText; i++, text++ ) {
			if ( text->depthTest == depthTest ) {
				const dword color = opaque( text->color );
				RB_TextStrokes( text->text, text->origin, text->scale, text->viewAxis, text->align, [&builder, color]( const idVec3 & p1, const idVec3 & p2 ) {
					builder.Line( p1, p2, color );
				} );
			}
		}
	}

	// polygon offset does not move lines, so the outlines are plain depth tested lines
	if ( r_debugPolygonFilled.GetBool() ) {
		builder.Begin( primitive_t::TRIANGLES, GLS_POLYGON_OFFSET | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHMASK );
	} else {
		builder.Begin( primitive_t::LINES, depthTestBits );
	}
	const debugPolygon_t * poly = rb_debugPolygons;
	for ( int i = 0; i < rb_numDebugPolygons; i++, poly++ ) {
		builder.Polygon( poly->winding, PackColor( poly->rgb ) );
	}

	builder.Flush();
}

/*
================
RB_DrawDebugBatches

The debug lines, text and polygons of the frame, all in world space.
================
*/
static void RB_DrawDebugBatches() {
	if ( backEnd.numDebugBatches == 0 ) {
		return;
	}

	RB_ResetViewScissor();

	RB_SetMVP( backEnd.viewDef->worldSpace.mvp );
	backEnd.currentSpace = &backEnd.viewDef->worldSpace;

	renderBackend->Cull( CT_TWO_SIDED );
	RB_SetColor( 1.0f, 1.0f, 1.0f, 1.0f );
	RB_SetTextureColorParms( true );
	renderProgManager.BindShader_TextureVertexColor();

	renderBackend->PolygonOffset( -1.0f, -2.0f );

	for ( int i = 0; i < backEnd.numDebugBatches; i++ ) {
		const debugBatch_t & batch = backEnd.debugBatches[i];

		renderBackend->State( batch.stateBits );

		drawSurf_t surf = {};
		surf.ambientCache = batch.vertexCache;
		surf.indexCache = batch.indexCache;
		surf.numIndexes = batch.numIndexes;
		RB_DrawElementsWithCounters( &surf, batch.topology );
	}

	renderBackend->State( GLS_DEFAULT );
}

/*
================
RB_ShowCenterOfProjection
================
*/
void RB_ShowCenterOfProjection() {
	if ( !r_showCenterOfProjection.GetBool() ) {
		return;
	}

	renderBackend->State( GLS_DEFAULT );

	const int w = backEnd.viewDef->scissor.GetWidth();
	const int h = backEnd.viewDef->scissor.GetHeight();
	for ( float f = 0.0f ; f <= 1.0f ; f += 0.125f ) {
		RB_ClearRect( static_cast<int>( w * f - 1 ), 0, 3, h, colorRed );
		RB_ClearRect( 0, static_cast<int>( h * f - 1 ), w, 3, colorRed );
	}
	float f = 0.5f;
	RB_ClearRect( static_cast<int>( w * f - 1 ), 0, 3, h, colorGreen );
	RB_ClearRect( 0, static_cast<int>( h * f - 1 ), w, 3, colorGreen );

	RB_RestoreScissor();
}

/*
================
RB_ShowLines

Draw exact pixel lines to check pixel center sampling
================
*/
void RB_ShowLines() {
	if ( !r_showLines.GetBool() ) {
		return;
	}

	renderBackend->State( GLS_DEFAULT );

	idVec4 color;
	if ( backEnd.viewDef->renderView.viewEyeBuffer == 0 ) {
		color = colorRed;
	} else if ( backEnd.viewDef->renderView.viewEyeBuffer == 1 ) {
		color = colorGreen;
	} else {
		color = colorBlue;
	}

	const int start = ( r_showLines.GetInteger() > 2 );	// 1,3 = horizontal, 2,4 = vertical
	if ( r_showLines.GetInteger() == 1 || r_showLines.GetInteger() == 3 ) {
		for ( int i = start ; i < tr.GetHeight() ; i+=2 ) {
			RB_ClearRect( 0, i, tr.GetWidth(), 1, color );
		}
	} else {
		for ( int i = start ; i < tr.GetWidth() ; i+=2 ) {
			RB_ClearRect( i, 0, 1, tr.GetHeight(), color );
		}
	}

	RB_RestoreScissor();
}

/*
================
RB_TestImage

Display a single image over most of the screen
================
*/
void RB_TestImage() {
	idImage	*image = NULL;
	idImage *imageCr = NULL;
	idImage *imageCb = NULL;
	int		max;
	float	w, h;

	image = tr.testImage;
	if ( !image ) {
		return;
	}

	if ( tr.testVideo ) {
		cinData_t	cin;

		// R_TestVideo_f started it on the Sys_Milliseconds clock
		cin = tr.testVideo->ImageForTime( Sys_Milliseconds() );
		if ( cin.imageY != NULL ) {
			image = cin.imageY;
			imageCr = cin.imageCr;
			imageCb = cin.imageCb;
		} else {
			tr.testImage = NULL;
			return;
		}
		w = 0.25;
		h = 0.25;
	} else {
		max = image->GetUploadWidth() > image->GetUploadHeight() ? image->GetUploadWidth() : image->GetUploadHeight();

		w = 0.25 * image->GetUploadWidth() / max;
		h = 0.25 * image->GetUploadHeight() / max;

		w *= (float)renderSystem->GetHeight() / renderSystem->GetWidth();
	}

	// Set State
	renderBackend->State( GLS_DEPTHFUNC_ALWAYS | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO );
	renderBackend->Cull( CT_TWO_SIDED );

	// Set Parms
	float texS[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
	float texT[4] = { 0.0f, 1.0f, 0.0f, 0.0f };
	renderProgManager.SetRenderParm( RENDERPARM_TEXTUREMATRIX_S, texS );
	renderProgManager.SetRenderParm( RENDERPARM_TEXTUREMATRIX_T, texT );

	float texGenEnabled[4] = { 0, 0, 0, 0 };
	renderProgManager.SetRenderParm( RENDERPARM_TEXGEN_0_ENABLED, texGenEnabled );

	// not really necessary but just for clarity
	const float screenWidth = 1.0f;
	const float screenHeight = 1.0f;
	const float halfScreenWidth = screenWidth * 0.5f;
	const float halfScreenHeight = screenHeight * 0.5f;

	float scale[16] = { 0 };
	scale[0] = w; // scale
	scale[5] = h; // scale
	scale[12] = halfScreenWidth - ( halfScreenWidth * w ); // translate
	scale[13] = halfScreenHeight - ( halfScreenHeight * h ); // translate
	scale[10] = 1.0f;
	scale[15] = 1.0f;

	float ortho[16] = { 0 };
	ortho[0] = 2.0f / screenWidth;
	ortho[5] = -2.0f / screenHeight;
	ortho[10] = -2.0f;
	ortho[12] = -1.0f;
	ortho[13] = 1.0f;
	ortho[14] = -1.0f;
	ortho[15] = 1.0f;

	float finalOrtho[16];
	R_MatrixMultiply( scale, ortho, finalOrtho );

	float projMatrixTranspose[16];
	R_MatrixTranspose( finalOrtho, projMatrixTranspose );
	renderProgManager.SetRenderParms( RENDERPARM_MVPMATRIX_X, projMatrixTranspose, 4 );
	backEnd.currentSpace = NULL;

	// Set Color
	RB_SetColor( 1, 1, 1, 1 );

	// Bind the Texture
	if ( ( imageCr != NULL ) && ( imageCb != NULL ) ) {
		image->Bind( 0 );
		imageCr->Bind( 1 );
		imageCb->Bind( 2 );
		renderProgManager.BindShader_Bink();
	} else {
		image->Bind( 0 );
		// Set Shader
		renderProgManager.BindShader_Texture();
	}

	// Draw!
	RB_DrawElementsWithCounters( &backEnd.testImageSurface );
}

/*
=================
RB_RenderDebugTools
=================
*/
void RB_RenderDebugTools( drawSurf_t **drawSurfs, int numDrawSurfs ) {
	// don't do much if this was a 2D rendering
	if ( !backEnd.viewDef->viewEntitys ) {
		RB_TestImage();
		RB_ShowLines();
		return;
	}

	renderLog.OpenMainBlock( MRB_DRAW_DEBUG_TOOLS );
	RENDERLOG_PRINTF( "---------- RB_RenderDebugTools ----------\n" );

	renderBackend->State( GLS_DEFAULT );

	renderBackend->Scissor( backEnd.viewDef->viewport.x1 + backEnd.viewDef->scissor.x1,
				backEnd.viewDef->viewport.y1 + backEnd.viewDef->scissor.y1,
				backEnd.viewDef->scissor.x2 + 1 - backEnd.viewDef->scissor.x1,
				backEnd.viewDef->scissor.y2 + 1 - backEnd.viewDef->scissor.y1 );
	backEnd.currentScissor = backEnd.viewDef->scissor;

	RB_ShowLightCount();
	RB_ShowTris( drawSurfs, numDrawSurfs );
	RB_ShowLights();
	RB_TestImage();
	RB_ShowCenterOfProjection();
	RB_ShowLines();
	RB_DrawDebugBatches();

	renderLog.CloseMainBlock();
}

/*
=================
RB_ShutdownDebugTools
=================
*/
void RB_ShutdownDebugTools() {
	for ( int i = 0; i < MAX_DEBUG_POLYGONS; i++ ) {
		rb_debugPolygons[i].winding.Clear();
	}
	rb_frameDebugLines.Clear();
	rb_debugBatchBuilder.FreeMemory();
	backEnd.numDebugBatches = 0;
}
