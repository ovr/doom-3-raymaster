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

static void RB_DrawText( const char *text, const idVec3 &origin, float scale, const idVec4 &color, const idMat3 &viewAxis, const int align );

void RB_SetMVP( const idRenderMatrix & mvp );

/*
================
RB_DrawBounds
================
*/
void RB_DrawBounds( const idBounds &bounds ) {
	if ( bounds.IsCleared() ) {
		return;
	}
	qglBegin( GL_LINE_LOOP );
	qglVertex3f( bounds[0][0], bounds[0][1], bounds[0][2] );
	qglVertex3f( bounds[0][0], bounds[1][1], bounds[0][2] );
	qglVertex3f( bounds[1][0], bounds[1][1], bounds[0][2] );
	qglVertex3f( bounds[1][0], bounds[0][1], bounds[0][2] );
	qglEnd();
	qglBegin( GL_LINE_LOOP );
	qglVertex3f( bounds[0][0], bounds[0][1], bounds[1][2] );
	qglVertex3f( bounds[0][0], bounds[1][1], bounds[1][2] );
	qglVertex3f( bounds[1][0], bounds[1][1], bounds[1][2] );
	qglVertex3f( bounds[1][0], bounds[0][1], bounds[1][2] );
	qglEnd();

	qglBegin( GL_LINES );
	qglVertex3f( bounds[0][0], bounds[0][1], bounds[0][2] );
	qglVertex3f( bounds[0][0], bounds[0][1], bounds[1][2] );

	qglVertex3f( bounds[0][0], bounds[1][1], bounds[0][2] );
	qglVertex3f( bounds[0][0], bounds[1][1], bounds[1][2] );

	qglVertex3f( bounds[1][0], bounds[0][1], bounds[0][2] );
	qglVertex3f( bounds[1][0], bounds[0][1], bounds[1][2] );

	qglVertex3f( bounds[1][0], bounds[1][1], bounds[0][2] );
	qglVertex3f( bounds[1][0], bounds[1][1], bounds[1][2] );
	qglEnd();
}


/*
================
RB_SimpleSurfaceSetup
================
*/
static void RB_SimpleSurfaceSetup( const drawSurf_t *drawSurf ) {
	// change the matrix if needed
	if ( drawSurf->space != backEnd.currentSpace ) {
		qglLoadMatrixf( drawSurf->space->modelViewMatrix );
		backEnd.currentSpace = drawSurf->space;
	}

	// change the scissor if needed
	if ( !backEnd.currentScissor.Equals( drawSurf->scissorRect ) && r_useScissor.GetBool() ) {
		GL_Scissor( backEnd.viewDef->viewport.x1 + drawSurf->scissorRect.x1, 
					backEnd.viewDef->viewport.y1 + drawSurf->scissorRect.y1,
					drawSurf->scissorRect.x2 + 1 - drawSurf->scissorRect.x1,
					drawSurf->scissorRect.y2 + 1 - drawSurf->scissorRect.y1 );
		backEnd.currentScissor = drawSurf->scissorRect;
	}
}

/*
================
RB_SimpleWorldSetup
================
*/
static void RB_SimpleWorldSetup() {
	backEnd.currentSpace = &backEnd.viewDef->worldSpace;


	qglLoadMatrixf( backEnd.viewDef->worldSpace.modelViewMatrix );

	GL_Scissor( backEnd.viewDef->viewport.x1 + backEnd.viewDef->scissor.x1,
				backEnd.viewDef->viewport.y1 + backEnd.viewDef->scissor.y1,
				backEnd.viewDef->scissor.x2 + 1 - backEnd.viewDef->scissor.x1,
				backEnd.viewDef->scissor.y2 + 1 - backEnd.viewDef->scissor.y1 );
	backEnd.currentScissor = backEnd.viewDef->scissor;
}

/*
=================
RB_PolygonClear

This will cover the entire screen with normal rasterization.
Texturing is disabled, but the existing glColor, glDepthMask,
glColorMask, and the enabled state of depth buffering and
stenciling will matter.
=================
*/
void RB_PolygonClear() {
	qglPushMatrix();
	qglPushAttrib( GL_ALL_ATTRIB_BITS  );
	qglLoadIdentity();
	qglDisable( GL_TEXTURE_2D );
	qglDisable( GL_DEPTH_TEST );
	qglDisable( GL_CULL_FACE );
	qglDisable( GL_SCISSOR_TEST );
	qglBegin( GL_POLYGON );
	qglVertex3f( -20, -20, -10 );
	qglVertex3f( 20, -20, -10 );
	qglVertex3f( 20, 20, -10 );
	qglVertex3f( -20, 20, -10 );
	qglEnd();
	qglPopAttrib();
	qglPopMatrix();
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
	int		i;
	static float	colors[8][3] = {
		{0,0,0},
		{1,0,0},
		{0,1,0},
		{0,0,1},
		{0,1,1},
		{1,0,1},
		{1,1,0},
		{1,1,1},
	};

	// clear color buffer to white (>6 passes)
	GL_Clear( true, false, false, 0, 1.0f, 1.0f, 1.0f, 1.0f );

	// now draw color for each stencil value
	qglStencilOp( GL_KEEP, GL_KEEP, GL_KEEP );
	for ( i = 0; i < 6; i++ ) {
		GL_Color( colors[i] );
		renderProgManager.BindShader_Color();
		qglStencilFunc( GL_EQUAL, i, 255 );
		RB_PolygonClear();
	}

	qglStencilFunc( GL_ALWAYS, 0, 255 );
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

	RB_SimpleWorldSetup();

	GL_Clear( false, false, true, 0, 0.0f, 0.0f, 0.0f, 0.0f );

	// optionally count everything through walls
	if ( r_showLightCount.GetInteger() >= 2 ) {
		GL_State( GLS_DEPTHFUNC_EQUAL | GLS_STENCIL_OP_FAIL_KEEP | GLS_STENCIL_OP_ZFAIL_INCR | GLS_STENCIL_OP_PASS_INCR );
	} else {
		GL_State( GLS_DEPTHFUNC_EQUAL | GLS_STENCIL_OP_FAIL_KEEP | GLS_STENCIL_OP_ZFAIL_KEEP | GLS_STENCIL_OP_PASS_INCR );
	}

	globalImages->defaultImage->Bind();

	for ( vLight = backEnd.viewDef->viewLights; vLight; vLight = vLight->next ) {
		for ( i = 0; i < 2; i++ ) {
			for ( surf = i ? vLight->localInteractions: vLight->globalInteractions; surf; surf = (drawSurf_t *)surf->nextOnLight ) {
				RB_SimpleSurfaceSetup( surf );
				RB_DrawElementsWithCounters( surf );
			}
		}
	}

	// display the results
	R_ColorByStencilBuffer();
}

/*
===============
RB_EnterWeaponDepthHack
===============
*/
static void RB_EnterWeaponDepthHack() {
	float	matrix[16];

	memcpy( matrix, backEnd.viewDef->projectionMatrix, sizeof( matrix ) );

	const float modelDepthHack = 0.25f;
	matrix[2] *= modelDepthHack;
	matrix[6] *= modelDepthHack;
	matrix[10] *= modelDepthHack;
	matrix[14] *= modelDepthHack;

	qglMatrixMode( GL_PROJECTION );
	qglLoadMatrixf( matrix );
	qglMatrixMode( GL_MODELVIEW );
}

/*
===============
RB_EnterModelDepthHack
===============
*/
static void RB_EnterModelDepthHack( float depth ) {
	float matrix[16];

	memcpy( matrix, backEnd.viewDef->projectionMatrix, sizeof( matrix ) );

	matrix[14] -= depth;

	qglMatrixMode( GL_PROJECTION );
	qglLoadMatrixf( matrix );
	qglMatrixMode( GL_MODELVIEW );
}

/*
===============
RB_LeaveDepthHack
===============
*/
static void RB_LeaveDepthHack() {
	qglMatrixMode( GL_PROJECTION );
	qglLoadMatrixf( backEnd.viewDef->projectionMatrix );
	qglMatrixMode( GL_MODELVIEW );
}

/*
=============
RB_LoadMatrixWithBypass

does a glLoadMatrixf after optionally applying the low-latency bypass matrix
=============
*/
static void RB_LoadMatrixWithBypass( const float m[16] ) {
	glLoadMatrixf( m );
}

/*
====================
RB_RenderDrawSurfListWithFunction

The triangle functions can check backEnd.currentSpace != surf->space
to see if they need to perform any new matrix setup.  The modelview
matrix will already have been loaded, and backEnd.currentSpace will
be updated after the triangle function completes.
====================
*/
static void RB_RenderDrawSurfListWithFunction( drawSurf_t **drawSurfs, int numDrawSurfs, void (*triFunc_)( const drawSurf_t *) ) {
	backEnd.currentSpace = NULL;

	for ( int i = 0 ; i < numDrawSurfs ; i++ ) {
		const drawSurf_t * drawSurf = drawSurfs[i];
		if ( drawSurf == NULL ) {
			continue;
		}
		assert( drawSurf->space != NULL );
		if ( drawSurf->space != NULL ) {	// is it ever NULL?  Do we need to check?
			// change the matrix if needed
			if ( drawSurf->space != backEnd.currentSpace ) {
				RB_LoadMatrixWithBypass( drawSurf->space->modelViewMatrix );
			}

			if ( drawSurf->space->weaponDepthHack ) {
				RB_EnterWeaponDepthHack();
			}

			if ( drawSurf->space->modelDepthHack != 0.0f ) {
				RB_EnterModelDepthHack( drawSurf->space->modelDepthHack );
			}
		}

		// change the scissor if needed
		if ( r_useScissor.GetBool() && !backEnd.currentScissor.Equals( drawSurf->scissorRect ) ) {
			backEnd.currentScissor = drawSurf->scissorRect;
			GL_Scissor( backEnd.viewDef->viewport.x1 + backEnd.currentScissor.x1, 
				backEnd.viewDef->viewport.y1 + backEnd.currentScissor.y1,
				backEnd.currentScissor.x2 + 1 - backEnd.currentScissor.x1,
				backEnd.currentScissor.y2 + 1 - backEnd.currentScissor.y1 );
		}

		// render it
		triFunc_( drawSurf );

		if ( drawSurf->space != NULL && ( drawSurf->space->weaponDepthHack || drawSurf->space->modelDepthHack != 0.0f ) ) {
			RB_LeaveDepthHack();
		}

		backEnd.currentSpace = drawSurf->space;
	}
}

/*
=====================
RB_ShowTris

Debugging tool
=====================
*/
static void RB_ShowTris( drawSurf_t **drawSurfs, int numDrawSurfs ) {

	modelTrace_t mt;
	idVec3 end;

	if ( r_showTris.GetInteger() == 0 ) {
		return;
	}

	float color[4] = { 1, 1, 1, 1 };

	GL_PolygonOffset( -1.0f, -2.0f );
	
	switch ( r_showTris.GetInteger() ) {			
		case 1: // only draw visible ones
			GL_State( GLS_DEPTHMASK | GLS_ALPHAMASK | GLS_POLYMODE_LINE | GLS_POLYGON_OFFSET );
			break;
		case 2:	// draw all front facing
		case 3: // draw all
			GL_State( GLS_DEPTHMASK | GLS_ALPHAMASK | GLS_POLYMODE_LINE | GLS_POLYGON_OFFSET | GLS_DEPTHFUNC_ALWAYS );
			break;
		case 4: // only draw visible ones with blended lines
			GL_State( GLS_DEPTHMASK | GLS_ALPHAMASK | GLS_POLYMODE_LINE | GLS_POLYGON_OFFSET | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA );
			color[3] = 0.4f;
			break;
	}

	if ( r_showTris.GetInteger() == 3 ) {
		GL_Cull( CT_TWO_SIDED );
	}

	GL_Color( color );
	renderProgManager.BindShader_Color();

	RB_RenderDrawSurfListWithFunction( drawSurfs, numDrawSurfs, RB_DrawElementsWithCounters );

	if ( r_showTris.GetInteger() == 3 ) {
		GL_Cull( CT_FRONT_SIDED );
	}
}

/*
=====================
RB_ShowSurfaceInfo

Debugging tool
=====================
*/
static void RB_ShowSurfaceInfo( drawSurf_t **drawSurfs, int numDrawSurfs ) {
	modelTrace_t mt;
	idVec3 start, end;
	
	if ( !r_showSurfaceInfo.GetBool() ) {
		return;
	}

	// start far enough away that we don't hit the player model
	start = tr.primaryView->renderView.vieworg + tr.primaryView->renderView.viewaxis[0] * 16;
	end = start + tr.primaryView->renderView.viewaxis[0] * 1000.0f;
	if ( !tr.primaryWorld->Trace( mt, start, end, 0.0f, false ) ) {
		return;
	}

	qglDisable( GL_TEXTURE_2D );

	GL_Color( 1, 1, 1 );

	static float scale = -1;
	static float bias = -2;

	GL_PolygonOffset( scale, bias );
	GL_State( GLS_DEPTHFUNC_ALWAYS | GLS_POLYMODE_LINE | GLS_POLYGON_OFFSET );

	idVec3	trans[3];
	float	matrix[16];

	// transform the object verts into global space
	R_AxisToModelMatrix( mt.entity->axis, mt.entity->origin, matrix );

	tr.primaryWorld->DrawText( mt.entity->hModel->Name(), mt.point + tr.primaryView->renderView.viewaxis[2] * 12,
		0.35f, colorRed, tr.primaryView->renderView.viewaxis );
	tr.primaryWorld->DrawText( mt.material->GetName(), mt.point, 
		0.35f, colorBlue, tr.primaryView->renderView.viewaxis );
}

/*
=====================
RB_ShowViewEntitys

Debugging tool
=====================
*/
static void RB_ShowViewEntitys( viewEntity_t *vModels ) {
	if ( !r_showViewEntitys.GetBool() ) {
		return;
	}
	if ( r_showViewEntitys.GetInteger() >= 2 ) {
		common->Printf( "view entities: " );
		for ( const viewEntity_t * vModel = vModels; vModel; vModel = vModel->next ) {
			if ( vModel->entityDef->IsDirectlyVisible() ) {
				common->Printf( "<%i> ", vModel->entityDef->index );
			} else {
				common->Printf( "%i ", vModel->entityDef->index );
			}
		}
		common->Printf( "\n" );
	}

	renderProgManager.BindShader_Color();

	GL_Color( 1, 1, 1 );
	GL_State( GLS_DEPTHFUNC_ALWAYS | GLS_POLYMODE_LINE );
	GL_Cull( CT_TWO_SIDED );

	for ( const viewEntity_t * vModel = vModels; vModel; vModel = vModel->next ) {
		idBounds	b;

		qglLoadMatrixf( vModel->modelViewMatrix );

		const idRenderEntityLocal * edef = vModel->entityDef;
		if ( !edef ) {
			continue;
		}

		// draw the model bounds in white if directly visible,
		// or, blue if it is only-for-sahdow
		idVec4	color;
		if ( edef->IsDirectlyVisible() ) {
			color.Set( 1, 1, 1, 1 );
		} else {
			color.Set( 0, 0, 1, 1 );
		}
		GL_Color( color[0], color[1], color[2] );
		RB_DrawBounds( edef->localReferenceBounds );

		// transform the upper bounds corner into global space
		if ( r_showViewEntitys.GetInteger() >= 2 ) {
			idVec3 corner;
			R_LocalPointToGlobal( vModel->modelMatrix, edef->localReferenceBounds[1], corner );

			tr.primaryWorld->DrawText( 
				va( "%i:%s", edef->index, edef->parms.hModel->Name() ), 
				corner,
				0.25f, color, 
				tr.primaryView->renderView.viewaxis );
		}

		// draw the actual bounds in yellow if different
		if ( r_showViewEntitys.GetInteger() >= 3 ) {
			GL_Color( 1, 1, 0 );
			// FIXME: cannot instantiate a dynamic model from the renderer back-end
			idRenderModel *model = R_EntityDefDynamicModel( vModel->entityDef );
			if ( !model ) {
				continue;	// particles won't instantiate without a current view
			}
			b = model->Bounds( &vModel->entityDef->parms );
			if ( b != vModel->entityDef->localReferenceBounds ) {
				RB_DrawBounds( b );
			}
		}
	}
}

/*
=====================
RB_ShowNormals

Debugging tool
=====================
*/
static void RB_ShowNormals( drawSurf_t **drawSurfs, int numDrawSurfs ) {
	int			i, j;
	drawSurf_t	*drawSurf;
	idVec3		end;
	const srfTriangles_t	*tri;
	float		size;
	bool		showNumbers;
	idVec3		pos;

	if ( r_showNormals.GetFloat() == 0.0f ) {
		return;
	}

	if ( !r_debugLineDepthTest.GetBool() ) {
		GL_State( GLS_POLYMODE_LINE | GLS_DEPTHFUNC_ALWAYS );
	} else {
		GL_State( GLS_POLYMODE_LINE );
	}

	size = r_showNormals.GetFloat();
	if ( size < 0.0f ) {
		size = -size;
		showNumbers = true;
	} else {
		showNumbers = false;
	}

	for ( i = 0; i < numDrawSurfs; i++ ) {
		drawSurf = drawSurfs[i];

		RB_SimpleSurfaceSetup( drawSurf );

		tri = drawSurf->frontEndGeo;
		if ( !tri->verts ) {
			continue;
		}

		qglBegin( GL_LINES );
		for ( j = 0; j < tri->numVerts; j++ ) {
			const idVec3 normal = tri->verts[j].GetNormal();
			const idVec3 tangent = tri->verts[j].GetTangent();
			const idVec3 bitangent = tri->verts[j].GetBiTangent();
			GL_Color( 0, 0, 1 );
			qglVertex3fv( tri->verts[j].xyz.ToFloatPtr() );
			VectorMA( tri->verts[j].xyz, size, normal, end );
			qglVertex3fv( end.ToFloatPtr() );

			GL_Color( 1, 0, 0 );
			qglVertex3fv( tri->verts[j].xyz.ToFloatPtr() );
			VectorMA( tri->verts[j].xyz, size, tangent, end );
			qglVertex3fv( end.ToFloatPtr() );

			GL_Color( 0, 1, 0 );
			qglVertex3fv( tri->verts[j].xyz.ToFloatPtr() );
			VectorMA( tri->verts[j].xyz, size, bitangent, end );
			qglVertex3fv( end.ToFloatPtr() );
		}
		qglEnd();
	}

	if ( showNumbers ) {
		RB_SimpleWorldSetup();
		for ( i = 0; i < numDrawSurfs; i++ ) {
			drawSurf = drawSurfs[i];
			tri = drawSurf->frontEndGeo;
			if ( !tri->verts ) {
				continue;
			}
			
			for ( j = 0; j < tri->numVerts; j++ ) {
				const idVec3 normal = tri->verts[j].GetNormal();
				const idVec3 tangent = tri->verts[j].GetTangent();
				R_LocalPointToGlobal( drawSurf->space->modelMatrix, tri->verts[j].xyz + tangent + normal * 0.2f, pos );
				RB_DrawText( va( "%d", j ), pos, 0.01f, colorWhite, backEnd.viewDef->renderView.viewaxis, 1 );
			}

			for ( j = 0; j < tri->numIndexes; j += 3 ) {
				const idVec3 normal = tri->verts[ tri->indexes[ j + 0 ] ].GetNormal();
				R_LocalPointToGlobal( drawSurf->space->modelMatrix, ( tri->verts[ tri->indexes[ j + 0 ] ].xyz + tri->verts[ tri->indexes[ j + 1 ] ].xyz + tri->verts[ tri->indexes[ j + 2 ] ].xyz ) * ( 1.0f / 3.0f ) + normal * 0.2f, pos );
				RB_DrawText( va( "%d", j / 3 ), pos, 0.01f, colorCyan, backEnd.viewDef->renderView.viewaxis, 1 );
			}
		}
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

	GL_State( GLS_DEFAULT );

	// we use the 'vLight->invProjectMVPMatrix'
	qglMatrixMode( GL_PROJECTION );
	qglLoadIdentity(); 

	renderProgManager.BindShader_Color();

	GL_Cull( CT_TWO_SIDED );

	common->Printf( "volumes: " );	// FIXME: not in back end!

	int count = 0;
	for ( viewLight_t * vLight = backEnd.viewDef->viewLights; vLight != NULL; vLight = vLight->next ) {
		count++;

		// depth buffered planes
		if ( r_showLights.GetInteger() >= 2 ) {
			GL_State( GLS_DEPTHFUNC_ALWAYS | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHMASK );
			GL_Color( 0.0f, 0.0f, 1.0f, 0.25f );
			idRenderMatrix invProjectMVPMatrix;
			idRenderMatrix::Multiply( backEnd.viewDef->worldSpace.mvp, vLight->inverseBaseLightProject, invProjectMVPMatrix );
			RB_SetMVP( invProjectMVPMatrix );
			RB_DrawElementsWithCounters( &backEnd.zeroOneCubeSurface );
		}

		// non-hidden lines
		if ( r_showLights.GetInteger() >= 3 ) {
			GL_State( GLS_DEPTHFUNC_ALWAYS | GLS_POLYMODE_LINE | GLS_DEPTHMASK  );
			GL_Color( 1.0f, 1.0f, 1.0f );
			idRenderMatrix invProjectMVPMatrix;
			idRenderMatrix::Multiply( backEnd.viewDef->worldSpace.mvp, vLight->inverseBaseLightProject, invProjectMVPMatrix );
			RB_SetMVP( invProjectMVPMatrix );
			RB_DrawElementsWithCounters( &backEnd.zeroOneCubeSurface );
		}

		common->Printf( "%i ", vLight->lightDef->index );
	}

	common->Printf( " = %i total\n", count );

	// set back the default projection matrix
	qglMatrixMode( GL_PROJECTION );
	qglLoadMatrixf( backEnd.viewDef->projectionMatrix );
	qglMatrixMode( GL_MODELVIEW );
	qglLoadIdentity();
}

/*
=====================
RB_ShowPortals

Debugging tool, won't work correctly with SMP or when mirrors are present
=====================
*/
static void RB_ShowPortals() {
	if ( !r_showPortals.GetBool() ) {
		return;
	}

	// all portals are expressed in world coordinates
	RB_SimpleWorldSetup();

	renderProgManager.BindShader_Color();
	GL_State( GLS_DEPTHFUNC_ALWAYS );

	((idRenderWorldLocal *)backEnd.viewDef->renderWorld)->ShowPortals();
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
			len = strlen(text);
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
RB_DrawText

  oriented on the viewaxis
  align can be 0-left, 1-center (default), 2-right
================
*/
static void RB_DrawText( const char *text, const idVec3 &origin, float scale, const idVec4 &color, const idMat3 &viewAxis, const int align ) {
	renderProgManager.BindShader_Color();



	int i, j, len, num, index, charIndex, line;
	float textLen = 1.0f, spacing = 1.0f;
	idVec3 org, p1, p2;

	if ( text && *text ) {
		qglBegin( GL_LINES );
		qglColor3fv( color.ToFloatPtr() );

		if ( text[0] == '\n' ) {
			line = 1;
		} else {
			line = 0;
		}

		len = strlen( text );
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

				qglVertex3fv( p1.ToFloatPtr() );
				qglVertex3fv( p2.ToFloatPtr() );
			}
			org -= viewAxis[1] * ( spacing * scale );
		}

		qglEnd();
	}
}

/*
================
RB_ShowDebugText
================
*/
void RB_ShowDebugText() {
	int			i;
	debugText_t	*text;

	if ( !rb_numDebugText ) {
		return;
	}

	// all lines are expressed in world coordinates
	RB_SimpleWorldSetup();

	if ( !r_debugLineDepthTest.GetBool() ) {
		GL_State( GLS_POLYMODE_LINE | GLS_DEPTHFUNC_ALWAYS );
	} else {
		GL_State( GLS_POLYMODE_LINE );
	}

	text = rb_debugText;
	for ( i = 0; i < rb_numDebugText; i++, text++ ) {
		if ( !text->depthTest ) {
			RB_DrawText( text->text, text->origin, text->scale, text->color, text->viewAxis, text->align );
		}
	}

	if ( !r_debugLineDepthTest.GetBool() ) {
		GL_State( GLS_POLYMODE_LINE );
	}

	text = rb_debugText;
	for ( i = 0; i < rb_numDebugText; i++, text++ ) {
		if ( text->depthTest ) {
			RB_DrawText( text->text, text->origin, text->scale, text->color, text->viewAxis, text->align );
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
RB_ShowDebugLines
================
*/
void RB_ShowDebugLines() {
	int			i;
	debugLine_t	*line;

	if ( !rb_numDebugLines ) {
		return;
	}

	// all lines are expressed in world coordinates
	RB_SimpleWorldSetup();

	if ( !r_debugLineDepthTest.GetBool() ) {
		GL_State( GLS_POLYMODE_LINE | GLS_DEPTHFUNC_ALWAYS );
	} else {
		GL_State( GLS_POLYMODE_LINE );
	}

	qglBegin( GL_LINES );

	line = rb_debugLines;
	for ( i = 0; i < rb_numDebugLines; i++, line++ ) {
		if ( !line->depthTest ) {
			qglColor3fv( line->rgb.ToFloatPtr() );
			qglVertex3fv( line->start.ToFloatPtr() );
			qglVertex3fv( line->end.ToFloatPtr() );
		}
	}
	qglEnd();

	if ( !r_debugLineDepthTest.GetBool() ) {
		GL_State( GLS_POLYMODE_LINE );
	}

	qglBegin( GL_LINES );

	line = rb_debugLines;
	for ( i = 0; i < rb_numDebugLines; i++, line++ ) {
		if ( line->depthTest ) {
			qglColor4fv( line->rgb.ToFloatPtr() );
			qglVertex3fv( line->start.ToFloatPtr() );
			qglVertex3fv( line->end.ToFloatPtr() );
		}
	}

	qglEnd();

	GL_State( GLS_DEFAULT );
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
================
RB_ShowDebugPolygons
================
*/
void RB_ShowDebugPolygons() {
	int				i, j;
	debugPolygon_t	*poly;

	if ( !rb_numDebugPolygons ) {
		return;
	}

	// all lines are expressed in world coordinates
	RB_SimpleWorldSetup();

	qglDisable( GL_TEXTURE_2D );

	if ( r_debugPolygonFilled.GetBool() ) {
		GL_State( GLS_POLYGON_OFFSET | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHMASK );
		GL_PolygonOffset( -1, -2 );
	} else {
		GL_State( GLS_POLYGON_OFFSET | GLS_POLYMODE_LINE );
		GL_PolygonOffset( -1, -2 );
	}

	poly = rb_debugPolygons;
	for ( i = 0; i < rb_numDebugPolygons; i++, poly++ ) {
//		if ( !poly->depthTest ) {

			qglColor4fv( poly->rgb.ToFloatPtr() );

			qglBegin( GL_POLYGON );

			for ( j = 0; j < poly->winding.GetNumPoints(); j++) {
				qglVertex3fv( poly->winding[j].ToFloatPtr() );
			}

			qglEnd();
//		}
	}

	GL_State( GLS_DEFAULT );

	if ( r_debugPolygonFilled.GetBool() ) {
		qglDisable( GL_POLYGON_OFFSET_FILL );
	} else {
		qglDisable( GL_POLYGON_OFFSET_LINE );
	}

	GL_State( GLS_DEFAULT );
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

	const int w = backEnd.viewDef->scissor.GetWidth();
	const int h = backEnd.viewDef->scissor.GetHeight();
	qglClearColor( 1, 0, 0, 1 );
	for ( float f = 0.0f ; f <= 1.0f ; f += 0.125f ) {
		qglScissor( w * f - 1 , 0, 3, h );
		qglClear( GL_COLOR_BUFFER_BIT );
		qglScissor( 0, h * f - 1 , w, 3 );
		qglClear( GL_COLOR_BUFFER_BIT );
	}
	qglClearColor( 0, 1, 0, 1 );
	float f = 0.5f;
	qglScissor( w * f - 1 , 0, 3, h );
	qglClear( GL_COLOR_BUFFER_BIT );
	qglScissor( 0, h * f - 1 , w, 3 );
	qglClear( GL_COLOR_BUFFER_BIT );

	qglScissor( 0, 0, w, h );
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

	glEnable( GL_SCISSOR_TEST );
	if ( backEnd.viewDef->renderView.viewEyeBuffer == 0 ) {
		glClearColor( 1, 0, 0, 1 );
	} else if ( backEnd.viewDef->renderView.viewEyeBuffer == 1 ) {
		glClearColor( 0, 1, 0, 1 );
	} else {
		glClearColor( 0, 0, 1, 1 );
	}

	const int start = ( r_showLines.GetInteger() > 2 );	// 1,3 = horizontal, 2,4 = vertical
	if ( r_showLines.GetInteger() == 1 || r_showLines.GetInteger() == 3 ) {
		for ( int i = start ; i < tr.GetHeight() ; i+=2 ) {
			glScissor( 0, i, tr.GetWidth(), 1 );
			glClear( GL_COLOR_BUFFER_BIT );
		}
	} else {
		for ( int i = start ; i < tr.GetWidth() ; i+=2 ) {
			glScissor( i, 0, 1, tr.GetHeight() );
			glClear( GL_COLOR_BUFFER_BIT );
		}
	}
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
	GL_State( GLS_DEPTHFUNC_ALWAYS | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO );

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
	qglMatrixMode( GL_PROJECTION );
	qglLoadMatrixf( finalOrtho );
	qglMatrixMode( GL_MODELVIEW );
	qglLoadIdentity();

	// Set Color
	GL_Color( 1, 1, 1, 1 );

	// Bind the Texture
	if ( ( imageCr != NULL ) && ( imageCb != NULL ) ) {
		GL_SelectTexture( 0 );
		image->Bind();
		GL_SelectTexture( 1 );
		imageCr->Bind();
		GL_SelectTexture( 2 );
		imageCb->Bind();
		renderProgManager.BindShader_Bink();
	} else {
		GL_SelectTexture( 0 );
		image->Bind();
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

	GL_State( GLS_DEFAULT );

	GL_Scissor( backEnd.viewDef->viewport.x1 + backEnd.viewDef->scissor.x1,
				backEnd.viewDef->viewport.y1 + backEnd.viewDef->scissor.y1,
				backEnd.viewDef->scissor.x2 + 1 - backEnd.viewDef->scissor.x1,
				backEnd.viewDef->scissor.y2 + 1 - backEnd.viewDef->scissor.y1 );
	backEnd.currentScissor = backEnd.viewDef->scissor;

	RB_ShowLightCount();
	RB_ShowTris( drawSurfs, numDrawSurfs );
	RB_ShowSurfaceInfo( drawSurfs, numDrawSurfs );
	RB_ShowNormals( drawSurfs, numDrawSurfs );
	RB_ShowViewEntitys( backEnd.viewDef->viewEntitys );
	RB_ShowLights();
	RB_TestImage();
	RB_ShowPortals();
	RB_ShowCenterOfProjection();
	RB_ShowLines();
	RB_ShowDebugLines();
	RB_ShowDebugText();
	RB_ShowDebugPolygons();

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
}
