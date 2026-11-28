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

idCVar r_drawEyeColor( "r_drawEyeColor", "0", CVAR_RENDERER | CVAR_BOOL, "Draw a colored box, red = left eye, blue = right eye, grey = non-stereo" );
idCVar r_motionBlur( "r_motionBlur", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE, "1 - 5, log2 of the number of motion blur samples" );
idCVar r_forceZPassStencilShadows( "r_forceZPassStencilShadows", "0", CVAR_RENDERER | CVAR_BOOL, "force Z-pass rendering for performance testing" );
idCVar r_useStencilShadowPreload( "r_useStencilShadowPreload", "1", CVAR_RENDERER | CVAR_BOOL, "use stencil shadow preload algorithm instead of Z-fail" );
idCVar r_skipShaderPasses( "r_skipShaderPasses", "0", CVAR_RENDERER | CVAR_BOOL, "" );
idCVar r_skipInteractionFastPath( "r_skipInteractionFastPath", "1", CVAR_RENDERER | CVAR_BOOL, "" );
idCVar r_useLightStencilSelect( "r_useLightStencilSelect", "0", CVAR_RENDERER | CVAR_BOOL, "use stencil select pass" );

extern idCVar stereoRender_swapEyes;

backEndState_t	backEnd;

/*
====================
RB_SetColor
====================
*/
void RB_SetColor( float * color ) {
	if ( color == NULL ) {
		return;
	}
	RB_SetColor( color[0], color[1], color[2], color[3] );
}

/*
====================
RB_SetColor
====================
*/
void RB_SetColor( float r, float g, float b ) {
	RB_SetColor( r, g, b, 1.0f );
}

/*
====================
RB_SetColor
====================
*/
void RB_SetColor( float r, float g, float b, float a ) {
	const idVec4 parm = RB_ColorParm( idVec4( idMath::ClampFloat( 0.0f, 1.0f, r ), idMath::ClampFloat( 0.0f, 1.0f, g ),
		idMath::ClampFloat( 0.0f, 1.0f, b ), idMath::ClampFloat( 0.0f, 1.0f, a ) ) );
	renderProgManager.SetRenderParm( RENDERPARM_COLOR, parm.ToFloatPtr() );
}

// The fog's alpha is a distance and a modulate's alpha a weight: only rgb is a colour.
idVec4 RB_ColorParm( const idVec4 & color ) {
	if ( !backEnd.hdrPhase || backEnd.hdrGamma ) {
		return color;
	}
	return idVec4( R_SRGBToLinear( color.x ), R_SRGBToLinear( color.y ), R_SRGBToLinear( color.z ), color.w );
}

/*
================
SetVertexParm
================
*/
static ID_INLINE void SetVertexParm( renderParm_t rp, const float * value ) {
	renderProgManager.SetUniformValue( rp, value );
}

/*
================
SetVertexParms
================
*/
static ID_INLINE void SetVertexParms( renderParm_t rp, const float * value, int num ) {
	for ( int i = 0; i < num; i++ ) {
		renderProgManager.SetUniformValue( (renderParm_t)( rp + i ), value + ( i * 4 ) );
	}
}

/*
================
SetFragmentParm
================
*/
static ID_INLINE void SetFragmentParm( renderParm_t rp, const float * value ) {
	renderProgManager.SetUniformValue( rp, value );
}

/*
================
RB_SetMVP
================
*/
void RB_SetMVP( const idRenderMatrix & mvp ) { 
	SetVertexParms( RENDERPARM_MVPMATRIX_X, mvp[0], 4 );
}

/*
================
RB_SetMVPWithStereoOffset
================
*/
static void RB_SetMVPWithStereoOffset( const idRenderMatrix & mvp, const float stereoOffset ) { 
	idRenderMatrix offset = mvp;
	offset[0][3] += stereoOffset;

	SetVertexParms( RENDERPARM_MVPMATRIX_X, offset[0], 4 );
}

static const float zero[4] = { 0, 0, 0, 0 };
static const float one[4] = { 1, 1, 1, 1 };
static const float negOne[4] = { -1, -1, -1, -1 };

/*
================
RB_SetVertexColorParms
================
*/
static void RB_SetVertexColorParms( stageVertexColor_t svc ) {
	switch ( svc ) {
		case SVC_IGNORE:
			SetVertexParm( RENDERPARM_VERTEXCOLOR_MODULATE, zero );
			SetVertexParm( RENDERPARM_VERTEXCOLOR_ADD, one );
			break;
		case SVC_MODULATE:
			SetVertexParm( RENDERPARM_VERTEXCOLOR_MODULATE, one );
			SetVertexParm( RENDERPARM_VERTEXCOLOR_ADD, zero );
			break;
		case SVC_INVERSE_MODULATE:
			SetVertexParm( RENDERPARM_VERTEXCOLOR_MODULATE, negOne );
			SetVertexParm( RENDERPARM_VERTEXCOLOR_ADD, one );
			break;
	}
}

/*
================
RB_DrawElementsWithCounters
================
*/
void RB_DrawElementsWithCounters( const drawSurf_t *surf, primitive_t topology ) {
	drawCall_t dc = {};

	// get vertex buffer
	int vertOffset = 0;
	if ( !vertexCache.ResolveVertex( surf->ambientCache, dc.vertexBuffer, vertOffset ) ) {
		idLib::Warning( "RB_DrawElementsWithCounters, vertexBuffer == NULL" );
		return;
	}

	// get index buffer
	if ( !vertexCache.ResolveIndex( surf->indexCache, dc.indexBuffer, dc.indexOffset ) ) {
		idLib::Warning( "RB_DrawElementsWithCounters, indexBuffer == NULL" );
		return;
	}

	RENDERLOG_PRINTF( "Binding Buffers: %p:%i %p:%i\n", (void *)dc.vertexBuffer, vertOffset, (void *)dc.indexBuffer, dc.indexOffset );

	if ( surf->jointCache ) {
		if ( !verify( renderProgManager.ShaderUsesJoints() ) ) {
			return;
		}
	} else {
		if ( !verify( !renderProgManager.ShaderUsesJoints() || renderProgManager.ShaderHasOptionalSkinning() ) ) {
			return;
		}
	}


	if ( surf->jointCache ) {
		if ( !vertexCache.ResolveJoint( surf->jointCache, dc.jointBuffer, dc.jointOffset, dc.numJoints ) ) {
			idLib::Warning( "RB_DrawElementsWithCounters, jointBuffer == NULL" );
			return;
		}
		assert( ( dc.jointOffset & ( renderBackend->Caps().uniformBufferOffsetAlignment - 1 ) ) == 0 );
	}

	dc.baseVertex = vertOffset / sizeof ( idDrawVert );
	dc.numIndexes = r_singleTriangle.GetBool() ? 3 : surf->numIndexes;
	dc.layout = LAYOUT_DRAW_VERT;
	dc.topology = topology;
	dc.parms = renderProgManager.GetRenderParms();

	renderBackend->DrawIndexed( dc );

	backEnd.pc.c_drawElements++;
	backEnd.pc.c_drawIndexes += surf->numIndexes;
}

/*
======================
RB_GetShaderTextureMatrix
======================
*/
void RB_GetShaderTextureMatrix( const float *shaderRegisters, const textureStage_t *texture, float matrix[16] ) {
	matrix[0*4+0] = shaderRegisters[ texture->matrix[0][0] ];
	matrix[1*4+0] = shaderRegisters[ texture->matrix[0][1] ];
	matrix[2*4+0] = 0.0f;
	matrix[3*4+0] = shaderRegisters[ texture->matrix[0][2] ];

	matrix[0*4+1] = shaderRegisters[ texture->matrix[1][0] ];
	matrix[1*4+1] = shaderRegisters[ texture->matrix[1][1] ];
	matrix[2*4+1] = 0.0f;
	matrix[3*4+1] = shaderRegisters[ texture->matrix[1][2] ];

	// we attempt to keep scrolls from generating incredibly large texture values, but
	// center rotations and center scales can still generate offsets that need to be > 1
	if ( matrix[3*4+0] < -40.0f || matrix[12] > 40.0f ) {
		matrix[3*4+0] -= (int)matrix[3*4+0];
	}
	if ( matrix[13] < -40.0f || matrix[13] > 40.0f ) {
		matrix[13] -= (int)matrix[13];
	}

	matrix[0*4+2] = 0.0f;
	matrix[1*4+2] = 0.0f;
	matrix[2*4+2] = 1.0f;
	matrix[3*4+2] = 0.0f;

	matrix[0*4+3] = 0.0f;
	matrix[1*4+3] = 0.0f;
	matrix[2*4+3] = 0.0f;
	matrix[3*4+3] = 1.0f;
}

/*
======================
RB_LoadShaderTextureMatrix
======================
*/
static void RB_LoadShaderTextureMatrix( const float *shaderRegisters, const textureStage_t *texture ) {	
	float texS[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
	float texT[4] = { 0.0f, 1.0f, 0.0f, 0.0f };

	if ( texture->hasMatrix ) {
		float matrix[16];
		RB_GetShaderTextureMatrix( shaderRegisters, texture, matrix );
		texS[0] = matrix[0*4+0];
		texS[1] = matrix[1*4+0];
		texS[2] = matrix[2*4+0];
		texS[3] = matrix[3*4+0];
	
		texT[0] = matrix[0*4+1];
		texT[1] = matrix[1*4+1];
		texT[2] = matrix[2*4+1];
		texT[3] = matrix[3*4+1];

		RENDERLOG_PRINTF( "Setting Texture Matrix\n");
		renderLog.Indent();
		RENDERLOG_PRINTF( "Texture Matrix S : %4.3f, %4.3f, %4.3f, %4.3f\n", texS[0], texS[1], texS[2], texS[3] );
		RENDERLOG_PRINTF( "Texture Matrix T : %4.3f, %4.3f, %4.3f, %4.3f\n", texT[0], texT[1], texT[2], texT[3] );
		renderLog.Outdent();
	} 

	SetVertexParm( RENDERPARM_TEXTUREMATRIX_S, texS );
	SetVertexParm( RENDERPARM_TEXTUREMATRIX_T, texT );
}

/*
=====================
RB_BakeTextureMatrixIntoTexgen
=====================
*/
void RB_BakeTextureMatrixIntoTexgen( idPlane lightProject[3], const float *textureMatrix ) {
	float genMatrix[16];
	float final[16];

	genMatrix[0*4+0] = lightProject[0][0];
	genMatrix[1*4+0] = lightProject[0][1];
	genMatrix[2*4+0] = lightProject[0][2];
	genMatrix[3*4+0] = lightProject[0][3];

	genMatrix[0*4+1] = lightProject[1][0];
	genMatrix[1*4+1] = lightProject[1][1];
	genMatrix[2*4+1] = lightProject[1][2];
	genMatrix[3*4+1] = lightProject[1][3];

	genMatrix[0*4+2] = 0.0f;
	genMatrix[1*4+2] = 0.0f;
	genMatrix[2*4+2] = 0.0f;
	genMatrix[3*4+2] = 0.0f;

	genMatrix[0*4+3] = lightProject[2][0];
	genMatrix[1*4+3] = lightProject[2][1];
	genMatrix[2*4+3] = lightProject[2][2];
	genMatrix[3*4+3] = lightProject[2][3];

	R_MatrixMultiply( genMatrix, textureMatrix, final );

	lightProject[0][0] = final[0*4+0];
	lightProject[0][1] = final[1*4+0];
	lightProject[0][2] = final[2*4+0];
	lightProject[0][3] = final[3*4+0];

	lightProject[1][0] = final[0*4+1];
	lightProject[1][1] = final[1*4+1];
	lightProject[1][2] = final[2*4+1];
	lightProject[1][3] = final[3*4+1];
}

/*
======================
RB_BindVariableStageImage

Handles generating a cinematic frame if needed
======================
*/
static void RB_BindVariableStageImage( const textureStage_t *texture, const float *shaderRegisters ) {
	if ( texture->cinematic ) {
		cinData_t cin;

		if ( r_skipDynamicTextures.GetBool() ) {
			globalImages->defaultImage->Bind( 0 );
			return;
		}

		// offset time by shaderParm[7] (FIXME: make the time offset a parameter of the shader?)
		// We make no attempt to optimize for multiple identical cinematics being in view, or
		// for cinematics going at a lower framerate than the renderer.
		cin = texture->cinematic->ImageForTime( backEnd.viewDef->renderView.time[0] + idMath::Ftoi( 1000.0f * backEnd.viewDef->renderView.shaderParms[11] ) );
		if ( cin.imageY != NULL ) {
			cin.imageY->Bind( 0 );
			cin.imageCr->Bind( 1 );
			cin.imageCb->Bind( 2 );
		} else {
			globalImages->blackImage->Bind( 0 );
			// because the shaders may have already been set - we need to make sure we are not using a bink shader which would 
			// display incorrectly.  We may want to get rid of RB_BindVariableStageImage and inline the code so that the
			// SWF GUI case is handled better, too
			renderProgManager.BindShader_TextureVertexColor();
		}
	} else {
		// FIXME: see why image is invalid
		if ( texture->image != NULL ) {
			texture->image->Bind( 0 );
		}
	}
}

/*
================
RB_PrepareStageTexturing
================
*/
static void RB_PrepareStageTexturing( const shaderStage_t * pStage,  const drawSurf_t * surf ) {
	float useTexGenParm[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

	// set the texture matrix if needed
	RB_LoadShaderTextureMatrix( surf->shaderRegisters, &pStage->texture );

	// texgens
	if ( pStage->texture.texgen == TG_REFLECT_CUBE ) {

		// see if there is also a bump map specified
		const shaderStage_t *bumpStage = surf->material->GetBumpStage();
		if ( bumpStage != NULL ) {
			// per-pixel reflection mapping with bump mapping
			bumpStage->texture.image->Bind( 1 );

			RENDERLOG_PRINTF( "TexGen: TG_REFLECT_CUBE: Bumpy Environment\n" );
			if ( surf->jointCache ) {
				renderProgManager.BindShader_BumpyEnvironmentSkinned();
			} else {
				renderProgManager.BindShader_BumpyEnvironment();
			}
		} else {
			RENDERLOG_PRINTF( "TexGen: TG_REFLECT_CUBE: Environment\n" );
			if ( surf->jointCache ) {
				renderProgManager.BindShader_EnvironmentSkinned();
			} else {
				renderProgManager.BindShader_Environment();
			}
		}

	} else if ( pStage->texture.texgen == TG_SKYBOX_CUBE ) {

		renderProgManager.BindShader_SkyBox();

	} else if ( pStage->texture.texgen == TG_WOBBLESKY_CUBE ) {

		const int * parms = surf->material->GetTexGenRegisters();

		float wobbleDegrees = surf->shaderRegisters[ parms[0] ] * ( idMath::PI / 180.0f );
		float wobbleSpeed = surf->shaderRegisters[ parms[1] ] * ( 2.0f * idMath::PI / 60.0f );
		float rotateSpeed = surf->shaderRegisters[ parms[2] ] * ( 2.0f * idMath::PI / 60.0f );

		idVec3 axis[3];
		{
			// very ad-hoc "wobble" transform
			float s, c;
			idMath::SinCos( wobbleSpeed * backEnd.viewDef->renderView.time[0] * 0.001f, s, c );

			float ws, wc;
			idMath::SinCos( wobbleDegrees, ws, wc );

			axis[2][0] = ws * c;
			axis[2][1] = ws * s;
			axis[2][2] = wc;

			axis[1][0] = -s * s * ws;
			axis[1][2] = -s * ws * ws;
			axis[1][1] = idMath::Sqrt( idMath::Fabs( 1.0f - ( axis[1][0] * axis[1][0] + axis[1][2] * axis[1][2] ) ) );

			// make the second vector exactly perpendicular to the first
			axis[1] -= ( axis[2] * axis[1] ) * axis[2];
			axis[1].Normalize();

			// construct the third with a cross
			axis[0].Cross( axis[1], axis[2] );
		}

		// add the rotate
		float rs, rc;
		idMath::SinCos( rotateSpeed * backEnd.viewDef->renderView.time[0] * 0.001f, rs, rc );

		float transform[12];
		transform[0*4+0] = axis[0][0] * rc + axis[1][0] * rs;
		transform[0*4+1] = axis[0][1] * rc + axis[1][1] * rs;
		transform[0*4+2] = axis[0][2] * rc + axis[1][2] * rs;
		transform[0*4+3] = 0.0f;

		transform[1*4+0] = axis[1][0] * rc - axis[0][0] * rs;
		transform[1*4+1] = axis[1][1] * rc - axis[0][1] * rs;
		transform[1*4+2] = axis[1][2] * rc - axis[0][2] * rs;
		transform[1*4+3] = 0.0f;

		transform[2*4+0] = axis[2][0];
		transform[2*4+1] = axis[2][1];
		transform[2*4+2] = axis[2][2];
		transform[2*4+3] = 0.0f;

		SetVertexParms( RENDERPARM_WOBBLESKY_X, transform, 3 );
		renderProgManager.BindShader_WobbleSky();

	} else if ( ( pStage->texture.texgen == TG_SCREEN ) || ( pStage->texture.texgen == TG_SCREEN2 ) ) {

		useTexGenParm[0] = 1.0f;
		useTexGenParm[1] = 1.0f;
		useTexGenParm[2] = 1.0f;
		useTexGenParm[3] = 1.0f;

		float mat[16];
		R_MatrixMultiply( surf->space->modelViewMatrix, backEnd.viewDef->projectionMatrix, mat );

		RENDERLOG_PRINTF( "TexGen : %s\n", ( pStage->texture.texgen == TG_SCREEN ) ? "TG_SCREEN" : "TG_SCREEN2" );
		renderLog.Indent();

		float plane[4];
		plane[0] = mat[0*4+0];
		plane[1] = mat[1*4+0];
		plane[2] = mat[2*4+0];
		plane[3] = mat[3*4+0];
		SetVertexParm( RENDERPARM_TEXGEN_0_S, plane );
		RENDERLOG_PRINTF( "TEXGEN_S = %4.3f, %4.3f, %4.3f, %4.3f\n",  plane[0], plane[1], plane[2], plane[3] );

		plane[0] = mat[0*4+1];
		plane[1] = mat[1*4+1];
		plane[2] = mat[2*4+1];
		plane[3] = mat[3*4+1];
		SetVertexParm( RENDERPARM_TEXGEN_0_T, plane );
		RENDERLOG_PRINTF( "TEXGEN_T = %4.3f, %4.3f, %4.3f, %4.3f\n",  plane[0], plane[1], plane[2], plane[3] );

		plane[0] = mat[0*4+3];
		plane[1] = mat[1*4+3];
		plane[2] = mat[2*4+3];
		plane[3] = mat[3*4+3];
		SetVertexParm( RENDERPARM_TEXGEN_0_Q, plane );	
		RENDERLOG_PRINTF( "TEXGEN_Q = %4.3f, %4.3f, %4.3f, %4.3f\n",  plane[0], plane[1], plane[2], plane[3] );

		renderLog.Outdent();

	} else if ( pStage->texture.texgen == TG_DIFFUSE_CUBE ) {

		// As far as I can tell, this is never used
		idLib::Warning( "Using Diffuse Cube! Please contact Brian!" );

	} else if ( pStage->texture.texgen == TG_GLASSWARP ) {

		// As far as I can tell, this is never used
		idLib::Warning( "Using GlassWarp! Please contact Brian!" );
	}

	SetVertexParm( RENDERPARM_TEXGEN_0_ENABLED, useTexGenParm );
}

/*
================
RB_FinishStageTexturing
================
*/
static void RB_FinishStageTexturing( const shaderStage_t *pStage, const drawSurf_t *surf ) {

	if ( pStage->texture.texgen == TG_REFLECT_CUBE ) {
		renderProgManager.Unbind();
	}
}

/*
=========================================================================================

DEPTH BUFFER RENDERING

=========================================================================================
*/

/*
==================
RB_FillDepthBufferGeneric
==================
*/
static void RB_FillDepthBufferGeneric( const drawSurf_t * const * drawSurfs, int numDrawSurfs ) {
	for ( int i = 0; i < numDrawSurfs; i++ ) {
		const drawSurf_t * drawSurf = drawSurfs[i];
		const idMaterial * shader = drawSurf->material;

		// translucent surfaces don't put anything in the depth buffer and don't
		// test against it, which makes them fail the mirror clip plane operation
		if ( shader->Coverage() == MC_TRANSLUCENT ) {
			continue;
		}

		// get the expressions for conditionals / color / texcoords
		const float * regs = drawSurf->shaderRegisters;

		// if all stages of a material have been conditioned off, don't do anything
		int stage = 0;
		for ( ; stage < shader->GetNumStages(); stage++ ) {		
			const shaderStage_t * pStage = shader->GetStage( stage );
			// check the stage enable condition
			if ( regs[ pStage->conditionRegister ] != 0 ) {
				break;
			}
		}
		if ( stage == shader->GetNumStages() ) {
			continue;
		}

		// change the matrix if needed
		if ( drawSurf->space != backEnd.currentSpace ) {
			RB_SetMVP( drawSurf->space->mvp );

			backEnd.currentSpace = drawSurf->space;
		}

		uint64 surfGLState = 0;

		// set polygon offset if necessary
		if ( shader->TestMaterialFlag( MF_POLYGONOFFSET ) ) {
			surfGLState |= GLS_POLYGON_OFFSET;
			renderBackend->PolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset() );
		}

		// subviews will just down-modulate the color buffer
		float color[4];
		if ( shader->GetSort() == static_cast< float >( SS_SUBVIEW ) ) {
			surfGLState |= GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO | GLS_DEPTHFUNC_LESS;
			color[0] = 1.0f;
			color[1] = 1.0f;
			color[2] = 1.0f;
			color[3] = 1.0f;
		} else {
			// others just draw black
			color[0] = 0.0f;
			color[1] = 0.0f;
			color[2] = 0.0f;
			color[3] = 1.0f;
		}

		renderLog.OpenBlock( shader->GetName() );

		bool drawSolid = false;
		if ( shader->Coverage() == MC_OPAQUE ) {
			drawSolid = true;
		} else if ( shader->Coverage() == MC_PERFORATED ) {
			// we may have multiple alpha tested stages
			// if the only alpha tested stages are condition register omitted,
			// draw a normal opaque surface
			bool didDraw = false;

			// perforated surfaces may have multiple alpha tested stages
			for ( stage = 0; stage < shader->GetNumStages(); stage++ ) {		
				const shaderStage_t *pStage = shader->GetStage(stage);

				if ( !pStage->hasAlphaTest ) {
					continue;
				}

				// check the stage enable condition
				if ( regs[ pStage->conditionRegister ] == 0 ) {
					continue;
				}

				// if we at least tried to draw an alpha tested stage,
				// we won't draw the opaque surface
				didDraw = true;

				// set the alpha modulate
				color[3] = regs[ pStage->color.registers[3] ];

				// skip the entire stage if alpha would be black
				if ( color[3] <= 0.0f ) {
					continue;
				}

				uint64 stageGLState = surfGLState;

				// set privatePolygonOffset if necessary
				if ( pStage->privatePolygonOffset ) {
					renderBackend->PolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * pStage->privatePolygonOffset );
					stageGLState |= GLS_POLYGON_OFFSET;
				}

				RB_SetColor( color );

				renderBackend->State( stageGLState );
				idVec4 alphaTestValue( regs[ pStage->alphaTestRegister ] );
				SetFragmentParm( RENDERPARM_ALPHA_TEST, alphaTestValue.ToFloatPtr() );

				if ( drawSurf->jointCache ) {
					renderProgManager.BindShader_TextureVertexColorSkinned();
				} else {
					renderProgManager.BindShader_TextureVertexColor();
				}

				RB_SetVertexColorParms( SVC_IGNORE );

				// bind the texture
				pStage->texture.image->Bind( 0 );

				// set texture matrix and texGens
				RB_PrepareStageTexturing( pStage, drawSurf );

				// must render with less-equal for Z-Cull to work properly
				assert( ( renderBackend->GetStateBits() & GLS_DEPTHFUNC_BITS ) == GLS_DEPTHFUNC_LESS );

				// draw it
				RB_DrawElementsWithCounters( drawSurf );

				// clean up
				RB_FinishStageTexturing( pStage, drawSurf );

				// unset privatePolygonOffset if necessary
				if ( pStage->privatePolygonOffset ) {
					renderBackend->PolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset() );
				}
			}

			if ( !didDraw ) {
				drawSolid = true;
			}
		}

		// draw the entire surface solid
		if ( drawSolid ) {
			if ( shader->GetSort() == static_cast< float >( SS_SUBVIEW ) ) {
				renderProgManager.BindShader_Color();
				RB_SetColor( color );
				renderBackend->State( surfGLState );
			} else {
				if ( drawSurf->jointCache ) {
					renderProgManager.BindShader_DepthSkinned();
				} else {
					renderProgManager.BindShader_Depth();
				}
				renderBackend->State( surfGLState | GLS_ALPHAMASK );
			}

			// must render with less-equal for Z-Cull to work properly
			assert( ( renderBackend->GetStateBits() & GLS_DEPTHFUNC_BITS ) == GLS_DEPTHFUNC_LESS );

			// draw it
			RB_DrawElementsWithCounters( drawSurf );
		}

		renderLog.CloseBlock();
	}

	SetFragmentParm( RENDERPARM_ALPHA_TEST, vec4_zero.ToFloatPtr() );
}

/*
=====================
RB_FillDepthBufferFast

Optimized fast path code.

If there are subview surfaces, they must be guarded in the depth buffer to allow
the mirror / subview to show through underneath the current view rendering.

Surfaces with perforated shaders need the full shader setup done, but should be
drawn after the opaque surfaces.

The bulk of the surfaces should be simple opaque geometry that can be drawn very rapidly.

If there are no subview surfaces, we could clear to black and use fast-Z rendering
on the 360.
=====================
*/
static void RB_FillDepthBufferFast( drawSurf_t **drawSurfs, int numDrawSurfs ) {
	if ( numDrawSurfs == 0 ) {
		return;
	}

	// if we are just doing 2D rendering, no need to fill the depth buffer
	if ( backEnd.viewDef->viewEntitys == NULL ) {
		return;
	}

	renderLog.OpenMainBlock( MRB_FILL_DEPTH_BUFFER );
	renderLog.OpenBlock( "RB_FillDepthBufferFast" );

	// force MVP change on first surface
	backEnd.currentSpace = NULL;

	// draw all the subview surfaces, which will already be at the start of the sorted list,
	// with the general purpose path
	renderBackend->State( GLS_DEFAULT );

	int	surfNum;
	for ( surfNum = 0; surfNum < numDrawSurfs; surfNum++ ) {
		if ( drawSurfs[surfNum]->material->GetSort() != static_cast< float >( SS_SUBVIEW ) ) {
			break;
		}
		RB_FillDepthBufferGeneric( &drawSurfs[surfNum], 1 );
	}

	const drawSurf_t ** perforatedSurfaces = (const drawSurf_t ** )_alloca( numDrawSurfs * sizeof( drawSurf_t * ) );
	int numPerforatedSurfaces = 0;

	// draw all the opaque surfaces and build up a list of perforated surfaces that
	// we will defer drawing until all opaque surfaces are done
	renderBackend->State( GLS_DEFAULT );

	// continue checking past the subview surfaces
	for ( ; surfNum < numDrawSurfs; surfNum++ ) {
		const drawSurf_t * surf = drawSurfs[ surfNum ];
		const idMaterial * shader = surf->material;

		// translucent surfaces don't put anything in the depth buffer
		if ( shader->Coverage() == MC_TRANSLUCENT ) {
			continue;
		}
		if ( shader->Coverage() == MC_PERFORATED ) {
			// save for later drawing
			perforatedSurfaces[ numPerforatedSurfaces ] = surf;
			numPerforatedSurfaces++;
			continue;
		}

		// set polygon offset?

		// set mvp matrix
		if ( surf->space != backEnd.currentSpace ) {
			RB_SetMVP( surf->space->mvp );
			backEnd.currentSpace = surf->space;
		}

		renderLog.OpenBlock( shader->GetName() );

		if ( surf->jointCache ) {
			renderProgManager.BindShader_DepthSkinned();
		} else {
			renderProgManager.BindShader_Depth();
		}

		// must render with less-equal for Z-Cull to work properly
		assert( ( renderBackend->GetStateBits() & GLS_DEPTHFUNC_BITS ) == GLS_DEPTHFUNC_LESS );

		// draw it solid
		RB_DrawElementsWithCounters( surf );

		renderLog.CloseBlock();
	}

	// draw all perforated surfaces with the general code path
	if ( numPerforatedSurfaces > 0 ) {
		RB_FillDepthBufferGeneric( perforatedSurfaces, numPerforatedSurfaces );
	}

	renderLog.CloseBlock();
	renderLog.CloseMainBlock();
}

/*
=========================================================================================

GENERAL INTERACTION RENDERING

=========================================================================================
*/

const int INTERACTION_TEXUNIT_BUMP			= 0;
const int INTERACTION_TEXUNIT_FALLOFF		= 1;
const int INTERACTION_TEXUNIT_PROJECTION	= 2;
const int INTERACTION_TEXUNIT_DIFFUSE		= 3;
const int INTERACTION_TEXUNIT_SPECULAR		= 4;

/*
==================
RB_SetupInteractionStage
==================
*/
void RB_SetupInteractionStage( const shaderStage_t *surfaceStage, const float *surfaceRegs, const float lightColor[4],
									idVec4 matrix[2], float color[4] ) {

	if ( surfaceStage->texture.hasMatrix ) {
		matrix[0][0] = surfaceRegs[surfaceStage->texture.matrix[0][0]];
		matrix[0][1] = surfaceRegs[surfaceStage->texture.matrix[0][1]];
		matrix[0][2] = 0.0f;
		matrix[0][3] = surfaceRegs[surfaceStage->texture.matrix[0][2]];

		matrix[1][0] = surfaceRegs[surfaceStage->texture.matrix[1][0]];
		matrix[1][1] = surfaceRegs[surfaceStage->texture.matrix[1][1]];
		matrix[1][2] = 0.0f;
		matrix[1][3] = surfaceRegs[surfaceStage->texture.matrix[1][2]];

		// we attempt to keep scrolls from generating incredibly large texture values, but
		// center rotations and center scales can still generate offsets that need to be > 1
		if ( matrix[0][3] < -40.0f || matrix[0][3] > 40.0f ) {
			matrix[0][3] -= idMath::Ftoi( matrix[0][3] );
		}
		if ( matrix[1][3] < -40.0f || matrix[1][3] > 40.0f ) {
			matrix[1][3] -= idMath::Ftoi( matrix[1][3] );
		}
	} else {
		matrix[0][0] = 1.0f;
		matrix[0][1] = 0.0f;
		matrix[0][2] = 0.0f;
		matrix[0][3] = 0.0f;

		matrix[1][0] = 0.0f;
		matrix[1][1] = 1.0f;
		matrix[1][2] = 0.0f;
		matrix[1][3] = 0.0f;
	}

	if ( color != NULL ) {
		for ( int i = 0; i < 4; i++ ) {
			// clamp here, so cards with a greater range don't look different.
			// we could perform overbrighting like we do for lights, but
			// it doesn't currently look worth it.
			color[i] = idMath::ClampFloat( 0.0f, 1.0f, surfaceRegs[surfaceStage->color.registers[i]] ) * lightColor[i];
		}
	}
}

/*
=========================================================================================

THE THIN G-BUFFER (R14)

=========================================================================================
*/

// The first bump, diffuse and specular stage that is on, as RB_RenderInteractions reads them; without one, flat or
// black (the black image is no YCoCg black: the colour 0 makes it one).
struct gbufferMaterial_t {
	idImage *	bump;
	idImage *	diffuse;
	idImage *	specular;
	idVec4		bumpMatrix[2] = { idVec4( 1, 0, 0, 0 ), idVec4( 0, 1, 0, 0 ) };
	idVec4		diffuseMatrix[2] = { idVec4( 1, 0, 0, 0 ), idVec4( 0, 1, 0, 0 ) };
	idVec4		specularMatrix[2] = { idVec4( 1, 0, 0, 0 ), idVec4( 0, 1, 0, 0 ) };
	idVec4		diffuseColor = vec4_zero;
	idVec4		specularColor = vec4_zero;
};

static gbufferMaterial_t RB_GBufferMaterial( const idMaterial & shader, const float * regs ) {
	gbufferMaterial_t material;
	material.bump = globalImages->flatNormalMap;
	material.diffuse = globalImages->blackImage;
	material.specular = globalImages->blackImage;
	if ( shader.GetFastPathBumpImage() != nullptr && !r_skipInteractionFastPath.GetBool() ) {
		material.bump = shader.GetFastPathBumpImage();
		material.diffuse = shader.GetFastPathDiffuseImage();
		material.diffuseColor = idVec4( 1.0f, 1.0f, 1.0f, 1.0f );
		if ( shader.GetFastPathSpecularImage() != nullptr ) {
			material.specular = shader.GetFastPathSpecularImage();
			material.specularColor = idVec4( 1.0f, 1.0f, 1.0f, 1.0f );
		}
		return material;
	}
	const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	bool bump = false;
	bool diffuse = false;
	bool specular = false;
	for ( int i = 0; i < shader.GetNumStages() && !( bump && diffuse && specular ); i++ ) {
		const shaderStage_t * stage = shader.GetStage( i );
		if ( regs[stage->conditionRegister] == 0 || stage->texture.image == nullptr ) {
			continue;
		}
		if ( stage->lighting == SL_BUMP && !bump ) {
			material.bump = stage->texture.image;
			RB_SetupInteractionStage( stage, regs, nullptr, material.bumpMatrix, nullptr );
			bump = true;
		} else if ( stage->lighting == SL_DIFFUSE && !diffuse ) {
			material.diffuse = stage->texture.image;
			RB_SetupInteractionStage( stage, regs, white, material.diffuseMatrix, material.diffuseColor.ToFloatPtr() );
			diffuse = true;
		} else if ( stage->lighting == SL_SPECULAR && !specular ) {
			material.specular = stage->texture.image;
			RB_SetupInteractionStage( stage, regs, white, material.specularMatrix, material.specularColor.ToFloatPtr() );
			specular = true;
		}
	}
	return material;
}

// The previous MVP in the order of R_AddSingleModel's, so a still entity under a still camera has no motion to the bit.
static void RB_SetGBufferSpace( const viewEntity_t & space, const idRenderMatrix & prevProjection, const float prevWorldModelView[16] ) {
	RB_SetMVP( space.mvp );
	float modelMatrixTranspose[16];
	R_MatrixTranspose( space.modelMatrix, modelMatrixTranspose );
	SetVertexParms( RENDERPARM_MODELMATRIX_X, modelMatrixTranspose, 4 );

	const float * prevModelMatrix = ( space.entityDef != nullptr ) ? space.prevModelMatrix : space.modelMatrix;
	float prevModelView[16];
	R_MatrixMultiply( prevModelMatrix, prevWorldModelView, prevModelView );
	idRenderMatrix prevView;
	idRenderMatrix::Transpose( *reinterpret_cast<const idRenderMatrix *>( prevModelView ), prevView );
	idRenderMatrix prevMvp;
	idRenderMatrix::Multiply( prevProjection, prevView, prevMvp );
	SetVertexParms( RENDERPARM_PREVMVPMATRIX_X, prevMvp[0], 4 );
}

// A polygon offset the depth pass writes a surface's depth at.
struct gbufferOffset_t {
	bool	enabled;
	float	units;
	bool	operator==( const gbufferOffset_t & ) const = default;
};
using gbufferOffsets_t = idStaticList<gbufferOffset_t, 8>;

// The polygon offsets the depth pass's generic path writes a surface at: the material's, or of a perforated surface
// those of the alpha-tested stages it draws, each once (a monster's two burn stages differ). None: it draws nothing.
static void RB_GBufferDepthOffsets( const idMaterial & shader, const float * regs, gbufferOffsets_t & offsets ) {
	offsets.Clear();
	int stage = 0;
	while ( stage < shader.GetNumStages() && regs[shader.GetStage( stage )->conditionRegister] == 0 ) {
		stage++;
	}
	if ( stage == shader.GetNumStages() ) {
		return;
	}
	const gbufferOffset_t whole = { shader.TestMaterialFlag( MF_POLYGONOFFSET ), shader.GetPolygonOffset() };
	bool tried = false;
	if ( shader.Coverage() == MC_PERFORATED ) {
		for ( stage = 0; stage < shader.GetNumStages(); stage++ ) {
			const shaderStage_t * pStage = shader.GetStage( stage );
			if ( !pStage->hasAlphaTest || regs[pStage->conditionRegister] == 0 ) {
				continue;
			}
			tried = true;
			if ( regs[pStage->color.registers[3]] <= 0.0f ) {
				continue;
			}
			const gbufferOffset_t own = pStage->privatePolygonOffset ? gbufferOffset_t{ true, pStage->privatePolygonOffset } : whole;
			if ( offsets.FindIndex( own ) < 0 && offsets.Num() < offsets.Max() ) {
				offsets.Append( own );
			}
		}
	}
	// without an alpha-tested stage that it tried, it draws the surface solid
	if ( !tried ) {
		offsets.Append( whole );
	}
}

/*
=====================
RB_FillGBuffer

The surfaces of the depth pass again, depth-equal without depth writes, into the thin G-buffer: each with the
depth pass's matrix, cull and polygon offsets (one draw per offset) lands on its own depth. The motion is from the
previous frame's camera and model matrix (and joints) to this one's.
=====================
*/
static void RB_FillGBuffer( drawSurf_t ** drawSurfs, int numDrawSurfs ) {
	const viewDef_t * viewDef = backEnd.viewDef;
	const idScreenRect & v = viewDef->viewport;
	if ( !renderBackend->BeginGBuffer( { v.x1, v.y1, v.GetWidth(), v.GetHeight() } ) ) {
		renderBackend->EndGBuffer();
		return;
	}
	renderLog.OpenMainBlock( MRB_GBUFFER );
	renderLog.OpenBlock( "RB_FillGBuffer" );

	// the camera of the eye's main view of the frame before when this one follows it, else this one: no camera motion
	const gbufferLastView_t & last = backEnd.gbufferLast[( viewDef->renderView.viewEyeBuffer == 1 ) ? 1 : 0];
	const bool moving = backEnd.viewFollows;
	const idRenderMatrix & prevProjection = moving ? last.projection : viewDef->projectionRenderMatrix;
	const float * prevWorldModelView = moving ? last.worldModelView : viewDef->worldSpace.modelViewMatrix;

	backEnd.currentSpace = nullptr;
	for ( int i = 0; i < numDrawSurfs; i++ ) {
		const drawSurf_t * surf = drawSurfs[i];
		const idMaterial & shader = *surf->material;
		if ( shader.Coverage() == MC_TRANSLUCENT ) {
			continue;
		}
		const float * regs = surf->shaderRegisters;
		const bool subview = ( shader.GetSort() == static_cast<float>( SS_SUBVIEW ) );
		// the fast path of the depth pass draws without an offset
		gbufferOffsets_t offsets;
		if ( subview || shader.Coverage() == MC_PERFORATED ) {
			RB_GBufferDepthOffsets( shader, regs, offsets );
		} else {
			offsets.Append( { false, 0.0f } );
		}
		if ( offsets.Num() == 0 ) {
			continue;
		}
		if ( surf->space != backEnd.currentSpace ) {
			RB_SetGBufferSpace( *surf->space, prevProjection, prevWorldModelView );
			backEnd.currentSpace = surf->space;
		}

		const gbufferMaterial_t material = RB_GBufferMaterial( shader, regs );
		SetVertexParm( RENDERPARM_BUMPMATRIX_S, material.bumpMatrix[0].ToFloatPtr() );
		SetVertexParm( RENDERPARM_BUMPMATRIX_T, material.bumpMatrix[1].ToFloatPtr() );
		SetVertexParm( RENDERPARM_DIFFUSEMATRIX_S, material.diffuseMatrix[0].ToFloatPtr() );
		SetVertexParm( RENDERPARM_DIFFUSEMATRIX_T, material.diffuseMatrix[1].ToFloatPtr() );
		SetVertexParm( RENDERPARM_SPECULARMATRIX_S, material.specularMatrix[0].ToFloatPtr() );
		SetVertexParm( RENDERPARM_SPECULARMATRIX_T, material.specularMatrix[1].ToFloatPtr() );
		SetFragmentParm( RENDERPARM_DIFFUSEMODIFIER, RB_ColorParm( material.diffuseColor ).ToFloatPtr() );
		SetFragmentParm( RENDERPARM_SPECULARMODIFIER, RB_ColorParm( material.specularColor ).ToFloatPtr() );

		// a mirror, the weapon and GUIs must not reflect the scene again
		const bool weapon = surf->space->weaponDepthHack;
		const bool reflects = !subview && !weapon && !surf->space->isGuiSurface;
		const bool twoSided = ( shader.GetCullType() == CT_TWO_SIDED ) || surf->space->isGuiSurface;
		const float gbufferParms[4] = { viewDef->rayTracing.gloss, static_cast<float>( ( reflects ? 1 : 0 ) | ( twoSided ? 2 : 0 ) | ( weapon ? 4 : 0 ) ),
			v.GetWidth() * 0.5f, v.GetHeight() * 0.5f };
		SetFragmentParm( RENDERPARM_GBUFFER, gbufferParms );
		if ( surf->jointCache ) {
			const float jointMotion[4] = { static_cast<float>( surf->prevJointRow ), 0.0f, 0.0f, 0.0f };
			SetVertexParm( RENDERPARM_JOINTMOTION, jointMotion );
			renderProgManager.BindShader_GBufferSkinned();
		} else {
			renderProgManager.BindShader_GBuffer();
		}
		material.bump->Bind( 0 );
		material.specular->Bind( 1 );
		material.diffuse->Bind( 2 );

		renderLog.OpenBlock( shader.GetName() );
		for ( int o = 0; o < offsets.Num(); o++ ) {
			const gbufferOffset_t & offset = offsets[o];
			if ( offset.enabled ) {
				renderBackend->PolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * offset.units );
			}
			renderBackend->State( GLS_DEPTHMASK | GLS_DEPTHFUNC_EQUAL | ( offset.enabled ? GLS_POLYGON_OFFSET : 0 ) );
			RB_DrawElementsWithCounters( surf );
		}
		renderLog.CloseBlock();
	}
	renderBackend->EndGBuffer();

	renderLog.CloseBlock();
	renderLog.CloseMainBlock();
}

/*
=================
RB_DrawSingleInteraction
=================
*/
static void RB_DrawSingleInteraction( drawInteraction_t * din ) {
	if ( din->bumpImage == NULL ) {
		// stage wasn't actually an interaction
		return;
	}

	if ( din->diffuseImage == NULL || r_skipDiffuse.GetBool() ) {
		// this isn't a YCoCg black, but it doesn't matter, because
		// the diffuseColor will also be 0
		din->diffuseImage = globalImages->blackImage;
	}
	if ( din->specularImage == NULL || r_skipSpecular.GetBool() || din->ambientLight ) {
		din->specularImage = globalImages->blackImage;
	}
	if ( r_skipBump.GetBool() ) {
		din->bumpImage = globalImages->flatNormalMap;
	}

	// if we wouldn't draw anything, don't call the Draw function
	const bool diffuseIsBlack = ( din->diffuseImage == globalImages->blackImage )
									|| ( ( din->diffuseColor[0] <= 0 ) && ( din->diffuseColor[1] <= 0 ) && ( din->diffuseColor[2] <= 0 ) );
	const bool specularIsBlack = ( din->specularImage == globalImages->blackImage )
									|| ( ( din->specularColor[0] <= 0 ) && ( din->specularColor[1] <= 0 ) && ( din->specularColor[2] <= 0 ) );
	if ( diffuseIsBlack && specularIsBlack ) {
		return;
	}

	// bump matrix
	SetVertexParm( RENDERPARM_BUMPMATRIX_S, din->bumpMatrix[0].ToFloatPtr() );
	SetVertexParm( RENDERPARM_BUMPMATRIX_T, din->bumpMatrix[1].ToFloatPtr() );

	// diffuse matrix
	SetVertexParm( RENDERPARM_DIFFUSEMATRIX_S, din->diffuseMatrix[0].ToFloatPtr() );
	SetVertexParm( RENDERPARM_DIFFUSEMATRIX_T, din->diffuseMatrix[1].ToFloatPtr() );

	// specular matrix
	SetVertexParm( RENDERPARM_SPECULARMATRIX_S, din->specularMatrix[0].ToFloatPtr() );
	SetVertexParm( RENDERPARM_SPECULARMATRIX_T, din->specularMatrix[1].ToFloatPtr() );

	RB_SetVertexColorParms( din->vertexColor );

	// the whole modifier (r_lightScale, the light, the surface, 2 for specular) decodes as one colour, so at
	// N.L = 1 one light gives the linear value of the LDR pixel
	SetFragmentParm( RENDERPARM_DIFFUSEMODIFIER, RB_ColorParm( din->diffuseColor ).ToFloatPtr() );
	SetFragmentParm( RENDERPARM_SPECULARMODIFIER, RB_ColorParm( din->specularColor ).ToFloatPtr() );

	// texture 0 will be the per-surface bump map
	din->bumpImage->Bind( INTERACTION_TEXUNIT_BUMP );

	// texture 3 is the per-surface diffuse map
	din->diffuseImage->Bind( INTERACTION_TEXUNIT_DIFFUSE );

	// texture 4 is the per-surface specular map
	din->specularImage->Bind( INTERACTION_TEXUNIT_SPECULAR );

	RB_DrawElementsWithCounters( din->surf );
}

/*
=================
RB_SetupForFastPathInteractions

These are common for all fast path surfaces
=================
*/
static void RB_SetupForFastPathInteractions( const idVec4 & diffuseColor, const idVec4 & specularColor ) {
	const idVec4 sMatrix( 1, 0, 0, 0 );
	const idVec4 tMatrix( 0, 1, 0, 0 );

	// bump matrix
	SetVertexParm( RENDERPARM_BUMPMATRIX_S, sMatrix.ToFloatPtr() );
	SetVertexParm( RENDERPARM_BUMPMATRIX_T, tMatrix.ToFloatPtr() );

	// diffuse matrix
	SetVertexParm( RENDERPARM_DIFFUSEMATRIX_S, sMatrix.ToFloatPtr() );
	SetVertexParm( RENDERPARM_DIFFUSEMATRIX_T, tMatrix.ToFloatPtr() );

	// specular matrix
	SetVertexParm( RENDERPARM_SPECULARMATRIX_S, sMatrix.ToFloatPtr() );
	SetVertexParm( RENDERPARM_SPECULARMATRIX_T, tMatrix.ToFloatPtr() );

	RB_SetVertexColorParms( SVC_IGNORE );

	SetFragmentParm( RENDERPARM_DIFFUSEMODIFIER, RB_ColorParm( diffuseColor ).ToFloatPtr() );
	SetFragmentParm( RENDERPARM_SPECULARMODIFIER, RB_ColorParm( specularColor ).ToFloatPtr() );
}

/*
=============
RB_RenderInteractions

With added sorting and trivial path work.
=============
*/
// A light whose interactions trace its shadows themselves (R17): interaction_rt in place of interaction.
struct tracedLight_t {
	uint32	mask;		// gpuSceneInstance_t::MASK_SHADOW*: the casters the surfaces drawn take shadows of
	int		noise;		// the index of the noise: the point of the light's disk a pixel aims at
	float	penumbra;	// rtShadowSettings_t::PENUMBRA_SCALE for NRD, else 0
};

static void RB_RenderInteractions( const drawSurf_t *surfList, const viewLight_t * vLight, int depthFunc, bool performStencilTest, bool useLightDepthBounds,
	const tracedLight_t * traced = nullptr ) {
	if ( surfList == NULL ) {
		return;
	}
	// ambient lights have a program of their own and cast no shadow
	assert( traced == nullptr || !vLight->lightShader->IsAmbientLight() );
	if ( traced != nullptr ) {
		const idVec3 & origin = vLight->globalLightOrigin;
		const float rtLight[4] = { origin.x, origin.y, origin.z, vLight->emitterRadius };
		const float rtShadow[4] = { static_cast<float>( traced->mask ), static_cast<float>( traced->noise ), traced->penumbra, 0.0f };
		SetFragmentParm( RENDERPARM_RT_LIGHT, rtLight );
		SetFragmentParm( RENDERPARM_RT_SHADOW, rtShadow );
	}

	// change the scissor if needed, it will be constant across all the surfaces lit by the light
	if ( !backEnd.currentScissor.Equals( vLight->scissorRect ) && r_useScissor.GetBool() ) {
		renderBackend->Scissor( backEnd.viewDef->viewport.x1 + vLight->scissorRect.x1, 
					backEnd.viewDef->viewport.y1 + vLight->scissorRect.y1,
					vLight->scissorRect.x2 + 1 - vLight->scissorRect.x1,
					vLight->scissorRect.y2 + 1 - vLight->scissorRect.y1 );
		backEnd.currentScissor = vLight->scissorRect;
	}

	// perform setup here that will be constant for all interactions
	if ( performStencilTest ) {
		renderBackend->State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHMASK | depthFunc | GLS_STENCIL_FUNC_EQUAL | GLS_STENCIL_MAKE_REF( STENCIL_SHADOW_TEST_VALUE ) | GLS_STENCIL_MAKE_MASK( STENCIL_SHADOW_MASK_VALUE ) );

	} else {
		renderBackend->State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHMASK | depthFunc | GLS_STENCIL_FUNC_ALWAYS );
	}

	// some rare lights have multiple animating stages, loop over them outside the surface list
	const idMaterial * lightShader = vLight->lightShader;
	const float * lightRegs = vLight->shaderRegisters;

	drawInteraction_t inter = {};
	inter.ambientLight = lightShader->IsAmbientLight();

	//---------------------------------
	// Split out the complex surfaces from the fast-path surfaces
	// so we can do the fast path ones all in a row.
	// The surfaces should already be sorted by space because they
	// are added single-threaded, and there is only a negligable amount
	// of benefit to trying to sort by materials.
	//---------------------------------
	static const int MAX_INTERACTIONS_PER_LIGHT = 1024;
	static const int MAX_COMPLEX_INTERACTIONS_PER_LIGHT = 128;
	idStaticList< const drawSurf_t *, MAX_INTERACTIONS_PER_LIGHT > allSurfaces;
	idStaticList< const drawSurf_t *, MAX_COMPLEX_INTERACTIONS_PER_LIGHT > complexSurfaces;
	for ( const drawSurf_t * walk = surfList; walk != NULL; walk = walk->nextOnLight ) {

		// make sure the triangle culling is done
		if ( walk->shadowVolumeState != SHADOWVOLUME_DONE ) {
			assert( walk->shadowVolumeState == SHADOWVOLUME_UNFINISHED || walk->shadowVolumeState == SHADOWVOLUME_DONE );

			uint64 start = Sys_Microseconds();
			while ( walk->shadowVolumeState == SHADOWVOLUME_UNFINISHED ) {
				Sys_Yield();
			}
			uint64 end = Sys_Microseconds();

			backEnd.pc.shadowMicroSec += end - start;
		}

		const idMaterial * surfaceShader = walk->material;
		if ( surfaceShader->GetFastPathBumpImage() ) {
			allSurfaces.Append( walk );
		} else {
			complexSurfaces.Append( walk );
		}
	}
	for ( int i = 0; i < complexSurfaces.Num(); i++ ) {
		allSurfaces.Append( complexSurfaces[i] );
	}

	bool lightDepthBoundsDisabled = false;

	for ( int lightStageNum = 0; lightStageNum < lightShader->GetNumStages(); lightStageNum++ ) {
		const shaderStage_t	*lightStage = lightShader->GetStage( lightStageNum );

		// ignore stages that fail the condition
		if ( !lightRegs[ lightStage->conditionRegister ] ) {
			continue;
		}

		// R19: with the indirect light on, the main view's stand-ins for it are dimmed
		const float fill = ( backEnd.gbufferView && vLight->fillLight ) ? backEnd.viewDef->rayTracing.fillLightScale : 1.0f;
		const float lightScale = r_lightScale.GetFloat() * fill;
		const idVec4 lightColor(
			lightScale * lightRegs[ lightStage->color.registers[0] ],
			lightScale * lightRegs[ lightStage->color.registers[1] ],
			lightScale * lightRegs[ lightStage->color.registers[2] ],
			lightRegs[ lightStage->color.registers[3] ] );
		// apply the world-global overbright and the 2x factor for specular
		const idVec4 diffuseColor = lightColor;
		const idVec4 specularColor = lightColor * 2.0f;

		float lightTextureMatrix[16];
		if ( lightStage->texture.hasMatrix ) {
			RB_GetShaderTextureMatrix( lightRegs, &lightStage->texture, lightTextureMatrix );
		}

		// texture 1 will be the light falloff texture
		vLight->falloffImage->Bind( INTERACTION_TEXUNIT_FALLOFF );

		// texture 2 will be the light projection texture
		lightStage->texture.image->Bind( INTERACTION_TEXUNIT_PROJECTION );

		// force the light textures to not use anisotropic filtering, which is wasted on them
		// all of the texture sampler parms should be constant for all interactions, only
		// the actual texture image bindings will change

		//----------------------------------
		// For all surfaces on this light list, generate an interaction for this light stage
		//----------------------------------

		// setup renderparms assuming we will be drawing trivial surfaces first
		RB_SetupForFastPathInteractions( diffuseColor, specularColor );

		// even if the space does not change between light stages, each light stage may need a different lightTextureMatrix baked in
		backEnd.currentSpace = NULL;

		for ( int sortedSurfNum = 0; sortedSurfNum < allSurfaces.Num(); sortedSurfNum++ ) {
			const drawSurf_t * const surf = allSurfaces[ sortedSurfNum ];

			// select the render prog
			if ( lightShader->IsAmbientLight() ) {
				if ( surf->jointCache ) {
					renderProgManager.BindShader_InteractionAmbientSkinned();
				} else {
					renderProgManager.BindShader_InteractionAmbient();
				}
			} else if ( traced != nullptr ) {
				if ( surf->jointCache ) {
					renderProgManager.BindShader_InteractionRtSkinned();
				} else {
					renderProgManager.BindShader_InteractionRt();
				}
			} else {
				if ( surf->jointCache ) {
					renderProgManager.BindShader_InteractionSkinned();
				} else {
					renderProgManager.BindShader_Interaction();
				}
			}

			const idMaterial * surfaceShader = surf->material;
			const float * surfaceRegs = surf->shaderRegisters;

			inter.surf = surf;

			// change the MVP matrix, view/light origin and light projection vectors if needed
			if ( surf->space != backEnd.currentSpace ) {
				backEnd.currentSpace = surf->space;

				// turn off the light depth bounds test if this model is rendered with a depth hack
				if ( useLightDepthBounds ) {
					if ( !surf->space->weaponDepthHack && surf->space->modelDepthHack == 0.0f ) {
						if ( lightDepthBoundsDisabled ) {
							renderBackend->DepthBoundsTest( vLight->scissorRect.zmin, vLight->scissorRect.zmax );
							lightDepthBoundsDisabled = false;
						}
					} else {
						if ( !lightDepthBoundsDisabled ) {
							renderBackend->DepthBoundsTest( 0.0f, 0.0f );
							lightDepthBoundsDisabled = true;
						}
					}
				}

				// model-view-projection
				RB_SetMVP( surf->space->mvp );

				// the ray of a traced shadow starts at the pixel's world position
				if ( traced != nullptr ) {
					float modelMatrixTranspose[16];
					R_MatrixTranspose( surf->space->modelMatrix, modelMatrixTranspose );
					SetVertexParms( RENDERPARM_MODELMATRIX_X, modelMatrixTranspose, 4 );
				}

				// tranform the light/view origin into model local space
				idVec4 localLightOrigin( 0.0f );
				idVec4 localViewOrigin( 1.0f );
				R_GlobalPointToLocal( surf->space->modelMatrix, vLight->globalLightOrigin, localLightOrigin.ToVec3() );
				R_GlobalPointToLocal( surf->space->modelMatrix, backEnd.viewDef->renderView.vieworg, localViewOrigin.ToVec3() );

				// set the local light/view origin
				SetVertexParm( RENDERPARM_LOCALLIGHTORIGIN, localLightOrigin.ToFloatPtr() );
				SetVertexParm( RENDERPARM_LOCALVIEWORIGIN, localViewOrigin.ToFloatPtr() );

				// transform the light project into model local space
				idPlane lightProjection[4];
				for ( int i = 0; i < 4; i++ ) {
					R_GlobalPlaneToLocal( surf->space->modelMatrix, vLight->lightProject[i], lightProjection[i] );
				}

				// optionally multiply the local light projection by the light texture matrix
				if ( lightStage->texture.hasMatrix ) {
					RB_BakeTextureMatrixIntoTexgen( lightProjection, lightTextureMatrix );
				}

				// set the light projection
				SetVertexParm( RENDERPARM_LIGHTPROJECTION_S, lightProjection[0].ToFloatPtr() );
				SetVertexParm( RENDERPARM_LIGHTPROJECTION_T, lightProjection[1].ToFloatPtr() );
				SetVertexParm( RENDERPARM_LIGHTPROJECTION_Q, lightProjection[2].ToFloatPtr() );
				SetVertexParm( RENDERPARM_LIGHTFALLOFF_S, lightProjection[3].ToFloatPtr() );
			}

			// check for the fast path
			if ( surfaceShader->GetFastPathBumpImage() && !r_skipInteractionFastPath.GetBool() ) {
				renderLog.OpenBlock( surf->material->GetName() );

				// texture 0 will be the per-surface bump map
				surfaceShader->GetFastPathBumpImage()->Bind( INTERACTION_TEXUNIT_BUMP );

				// texture 3 is the per-surface diffuse map
				surfaceShader->GetFastPathDiffuseImage()->Bind( INTERACTION_TEXUNIT_DIFFUSE );

				// texture 4 is the per-surface specular map
				surfaceShader->GetFastPathSpecularImage()->Bind( INTERACTION_TEXUNIT_SPECULAR );

				RB_DrawElementsWithCounters( surf );

				renderLog.CloseBlock();
				continue;
			}
			
			renderLog.OpenBlock( surf->material->GetName() );

			inter.bumpImage = NULL;
			inter.specularImage = NULL;
			inter.diffuseImage = NULL;
			inter.diffuseColor[0] = inter.diffuseColor[1] = inter.diffuseColor[2] = inter.diffuseColor[3] = 0;
			inter.specularColor[0] = inter.specularColor[1] = inter.specularColor[2] = inter.specularColor[3] = 0;

			// go through the individual surface stages
			//
			// This is somewhat arcane because of the old support for video cards that had to render
			// interactions in multiple passes.
			//
			// We also have the very rare case of some materials that have conditional interactions
			// for the "hell writing" that can be shined on them.
			for ( int surfaceStageNum = 0; surfaceStageNum < surfaceShader->GetNumStages(); surfaceStageNum++ ) {
				const shaderStage_t	*surfaceStage = surfaceShader->GetStage( surfaceStageNum );

				switch( surfaceStage->lighting ) {
					case SL_COVERAGE: {
						// ignore any coverage stages since they should only be used for the depth fill pass
						// for diffuse stages that use alpha test.
						break;
					}
					case SL_AMBIENT: {
						// ignore ambient stages while drawing interactions
						break;
					}
					case SL_BUMP: {
						// ignore stage that fails the condition
						if ( !surfaceRegs[ surfaceStage->conditionRegister ] ) {
							break;
						}
						// draw any previous interaction
						if ( inter.bumpImage != NULL ) {
							RB_DrawSingleInteraction( &inter );
						}
						inter.bumpImage = surfaceStage->texture.image;
						inter.diffuseImage = NULL;
						inter.specularImage = NULL;
						RB_SetupInteractionStage( surfaceStage, surfaceRegs, NULL,
												inter.bumpMatrix, NULL );
						break;
					}
					case SL_DIFFUSE: {
						// ignore stage that fails the condition
						if ( !surfaceRegs[ surfaceStage->conditionRegister ] ) {
							break;
						}
						// draw any previous interaction
						if ( inter.diffuseImage != NULL ) {
							RB_DrawSingleInteraction( &inter );
						}
						inter.diffuseImage = surfaceStage->texture.image;
						inter.vertexColor = surfaceStage->vertexColor;
						RB_SetupInteractionStage( surfaceStage, surfaceRegs, diffuseColor.ToFloatPtr(),
												inter.diffuseMatrix, inter.diffuseColor.ToFloatPtr() );
						break;
					}
					case SL_SPECULAR: {
						// ignore stage that fails the condition
						if ( !surfaceRegs[ surfaceStage->conditionRegister ] ) {
							break;
						}
						// draw any previous interaction
						if ( inter.specularImage != NULL ) {
							RB_DrawSingleInteraction( &inter );
						}
						inter.specularImage = surfaceStage->texture.image;
						inter.vertexColor = surfaceStage->vertexColor;
						RB_SetupInteractionStage( surfaceStage, surfaceRegs, specularColor.ToFloatPtr(),
												inter.specularMatrix, inter.specularColor.ToFloatPtr() );
						break;
					}
				}
			}

			// draw the final interaction
			RB_DrawSingleInteraction( &inter );

			renderLog.CloseBlock();
		}
	}

	if ( useLightDepthBounds && lightDepthBoundsDisabled ) {
		renderBackend->DepthBoundsTest( vLight->scissorRect.zmin, vLight->scissorRect.zmax );
	}

	renderProgManager.Unbind();
}

/*
==============================================================================================

STENCIL SHADOW RENDERING

==============================================================================================
*/

/*
=====================
RB_StencilShadowPass

The stencil buffer should have been set to 128 on any surfaces that might receive shadows.
=====================
*/
static void RB_StencilShadowPass( const drawSurf_t *drawSurfs, const viewLight_t * vLight ) {
	if ( r_skipShadows.GetBool() ) {
		return;
	}

	if ( drawSurfs == NULL ) {
		return;
	}

	RENDERLOG_PRINTF( "---------- RB_StencilShadowPass ----------\n" );

	renderProgManager.BindShader_Shadow();

	uint64 glState = 0;

	// for visualizing the shadows
	if ( r_showShadows.GetInteger() ) {
		// set the debug shadow color
		SetFragmentParm( RENDERPARM_COLOR, colorMagenta.ToFloatPtr() );
		if ( r_showShadows.GetInteger() == 2 ) {
			// draw filled in
			glState = GLS_DEPTHMASK | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_LESS;
		} else {
			// draw as lines, filling the depth buffer
			glState = GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO | GLS_POLYMODE_LINE | GLS_DEPTHFUNC_ALWAYS;
		}
	} else {
		// don't write to the color or depth buffer, just the stencil buffer
		glState = GLS_DEPTHMASK | GLS_COLORMASK | GLS_ALPHAMASK | GLS_DEPTHFUNC_LESS;
	}

	renderBackend->PolygonOffset( r_shadowPolygonFactor.GetFloat(), -r_shadowPolygonOffset.GetFloat() );

	// The pass starts with one-sided KEEP, KEEP, INCR; the surfaces below set their own two-sided ops.
	// Z-fail without the preload draws with whatever ops are set, so a shadow preset left by the
	// pass before is carried on: with state caching that request never reached GL.
	const uint64 latchedOps = renderBackend->GetStateBits() & GLS_STENCIL_OP_BITS;
	const bool carryOps = r_useStateCaching.GetBool() && GLS_IsStencilShadowOps( latchedOps );
	const uint64 passOps = carryOps ? latchedOps : ( GLS_STENCIL_OP_FAIL_KEEP | GLS_STENCIL_OP_ZFAIL_KEEP | GLS_STENCIL_OP_PASS_INCR );
	const uint64 passBits = glState | GLS_STENCIL_MAKE_REF( STENCIL_SHADOW_TEST_VALUE ) | GLS_STENCIL_MAKE_MASK( STENCIL_SHADOW_MASK_VALUE ) | GLS_POLYGON_OFFSET;
	renderBackend->State( passBits | passOps );

	// Two Sided Stencil reduces two draw calls to one for slightly faster shadows
	renderBackend->Cull( CT_TWO_SIDED );


	// process the chain of shadows with the current rendering state
	backEnd.currentSpace = NULL;

	for ( const drawSurf_t * drawSurf = drawSurfs; drawSurf != NULL; drawSurf = drawSurf->nextOnLight ) {
		if ( drawSurf->scissorRect.IsEmpty() ) {
			continue;	// !@# FIXME: find out why this is sometimes being hit!
						// temporarily jump over the scissor and draw so the gl error callback doesn't get hit
		}

		// make sure the shadow volume is done
		if ( drawSurf->shadowVolumeState != SHADOWVOLUME_DONE ) {
			assert( drawSurf->shadowVolumeState == SHADOWVOLUME_UNFINISHED || drawSurf->shadowVolumeState == SHADOWVOLUME_DONE );

			uint64 start = Sys_Microseconds();
			while ( drawSurf->shadowVolumeState == SHADOWVOLUME_UNFINISHED ) {
				Sys_Yield();
			}
			uint64 end = Sys_Microseconds();

			backEnd.pc.shadowMicroSec += end - start;
		}

		if ( drawSurf->numIndexes == 0 ) {
			continue;	// a job may have created an empty shadow volume
		}

		if ( !backEnd.currentScissor.Equals( drawSurf->scissorRect ) && r_useScissor.GetBool() ) {
			// change the scissor
			renderBackend->Scissor( backEnd.viewDef->viewport.x1 + drawSurf->scissorRect.x1,
						backEnd.viewDef->viewport.y1 + drawSurf->scissorRect.y1,
						drawSurf->scissorRect.x2 + 1 - drawSurf->scissorRect.x1,
						drawSurf->scissorRect.y2 + 1 - drawSurf->scissorRect.y1 );
			backEnd.currentScissor = drawSurf->scissorRect;
		}

		if ( drawSurf->space != backEnd.currentSpace ) {
			// change the matrix
			RB_SetMVP( drawSurf->space->mvp );

			// set the local light position to allow the vertex program to project the shadow volume end cap to infinity
			idVec4 localLight( 0.0f );
			R_GlobalPointToLocal( drawSurf->space->modelMatrix, vLight->globalLightOrigin, localLight.ToVec3() );
			SetVertexParm( RENDERPARM_LOCALLIGHTORIGIN, localLight.ToFloatPtr() );

			backEnd.currentSpace = drawSurf->space;
		}

		if ( r_showShadows.GetInteger() == 0 ) {
			if ( drawSurf->jointCache ) {
				renderProgManager.BindShader_ShadowSkinned();
			} else {
				renderProgManager.BindShader_Shadow();
			}
		} else {
			if ( drawSurf->jointCache ) {
				renderProgManager.BindShader_ShadowDebugSkinned();
			} else {
				renderProgManager.BindShader_ShadowDebug();
			}
		}

		// set depth bounds per shadow
		if ( r_useShadowDepthBounds.GetBool() ) {
			renderBackend->DepthBoundsTest( drawSurf->scissorRect.zmin, drawSurf->scissorRect.zmax );
		}

		// Determine whether or not the shadow volume needs to be rendered with Z-pass or
		// Z-fail. It is worthwhile to spend significant resources to reduce the number of
		// cases where shadow volumes need to be rendered with Z-fail because Z-fail
		// rendering can be significantly slower even on today's hardware. For instance,
		// on NVIDIA hardware Z-fail rendering causes the Z-Cull to be used in reverse:
		// Z-near becomes Z-far (trivial accept becomes trivial reject). Using the Z-Cull
		// in reverse is far less efficient because the Z-Cull only stores Z-near per 16x16
		// pixels while the Z-far is stored per 4x2 pixels. (The Z-near coallesce buffer
		// which has 4x4 granularity is only used when updating the depth which is not the
		// case for shadow volumes.) Note that it is also important to NOT use a Z-Cull
		// reconstruct because that would clear the Z-near of the Z-Cull which results in
		// no trivial rejection for Z-fail stencil shadow rendering.

		const bool renderZPass = ( drawSurf->renderZFail == 0 ) || r_forceZPassStencilShadows.GetBool();


		if ( renderZPass ) {
			// Z-pass
			renderBackend->State( passBits | GLS_STENCIL_SHADOW_ZPASS );
		} else if ( r_useStencilShadowPreload.GetBool() ) {
			// preload + Z-pass
			renderBackend->State( passBits | GLS_STENCIL_SHADOW_PRELOAD );
		} else {
			// Z-fail: the ops the last surface or the start of the pass left
			renderBackend->State( passBits | ( renderBackend->GetStateBits() & GLS_STENCIL_OP_BITS ) );
		}


		drawCall_t dc = {};

		// get vertex buffer
		int vertOffset = 0;
		if ( !vertexCache.ResolveVertex( drawSurf->shadowCache, dc.vertexBuffer, vertOffset ) ) {
			idLib::Warning( "RB_DrawElementsWithCounters, vertexBuffer == NULL" );
			continue;
		}

		// get index buffer
		if ( !vertexCache.ResolveIndex( drawSurf->indexCache, dc.indexBuffer, dc.indexOffset ) ) {
			idLib::Warning( "RB_DrawElementsWithCounters, indexBuffer == NULL" );
			continue;
		}

		RENDERLOG_PRINTF( "Binding Buffers: %p %p\n", (void *)dc.vertexBuffer, (void *)dc.indexBuffer );

		if ( drawSurf->jointCache ) {
			assert( renderProgManager.ShaderUsesJoints() );

			if ( !vertexCache.ResolveJoint( drawSurf->jointCache, dc.jointBuffer, dc.jointOffset, dc.numJoints ) ) {
				idLib::Warning( "RB_DrawElementsWithCounters, jointBuffer == NULL" );
				continue;
			}
			assert( ( dc.jointOffset & ( renderBackend->Caps().uniformBufferOffsetAlignment - 1 ) ) == 0 );

			dc.layout = LAYOUT_DRAW_SHADOW_VERT_SKINNED;
			dc.baseVertex = vertOffset / sizeof( idShadowVertSkinned );
		} else {
			dc.layout = LAYOUT_DRAW_SHADOW_VERT;
			dc.baseVertex = vertOffset / sizeof( idShadowVert );
		}

		dc.numIndexes = r_singleTriangle.GetBool() ? 3 : drawSurf->numIndexes;
		dc.topology = primitive_t::TRIANGLES;
		dc.parms = renderProgManager.GetRenderParms();

		renderBackend->DrawIndexed( dc );

		backEnd.pc.c_shadowElements++;
		backEnd.pc.c_shadowIndexes += drawSurf->numIndexes;

		if ( !renderZPass && r_useStencilShadowPreload.GetBool() ) {
			// render again with Z-pass: only the stencil ops differ
			renderBackend->State( passBits | GLS_STENCIL_SHADOW_ZPASS );

			dc.reuseParms = true;
			renderBackend->DrawIndexed( dc );
		}
	}

	// cleanup the shadow specific rendering state

	renderBackend->Cull( CT_FRONT_SIDED );

	// reset depth bounds
	if ( r_useShadowDepthBounds.GetBool() ) {
		if ( r_useLightDepthBounds.GetBool() ) {
			renderBackend->DepthBoundsTest( vLight->scissorRect.zmin, vLight->scissorRect.zmax );
		} else {
			renderBackend->DepthBoundsTest( 0.0f, 0.0f );
		}
	}
}

/*
==================
RB_StencilSelectLight

Deform the zeroOneCubeModel to exactly cover the light volume. Render the deformed cube model to the stencil buffer in
such a way that only fragments that are directly visible and contained within the volume will be written creating a 
mask to be used by the following stencil shadow and draw interaction passes.
==================
*/
static void RB_StencilSelectLight( const viewLight_t * vLight ) {
	renderLog.OpenBlock( "Stencil Select" );

	// enable the light scissor
	if ( !backEnd.currentScissor.Equals( vLight->scissorRect ) && r_useScissor.GetBool() ) {
		renderBackend->Scissor( backEnd.viewDef->viewport.x1 + vLight->scissorRect.x1, 
					backEnd.viewDef->viewport.y1 + vLight->scissorRect.y1,
					vLight->scissorRect.x2 + 1 - vLight->scissorRect.x1,
					vLight->scissorRect.y2 + 1 - vLight->scissorRect.y1 );
		backEnd.currentScissor = vLight->scissorRect;
	}

	// clear stencil buffer to 0 (not drawable)
	uint64 glStateMinusStencil = renderBackend->GetStateBits() & ~(GLS_STENCIL_OP_BITS|GLS_STENCIL_FUNC_BITS|GLS_STENCIL_FUNC_REF_BITS|GLS_STENCIL_FUNC_MASK_BITS);
	renderBackend->State( glStateMinusStencil | GLS_STENCIL_FUNC_ALWAYS | GLS_STENCIL_MAKE_REF( STENCIL_SHADOW_TEST_VALUE ) | GLS_STENCIL_MAKE_MASK( STENCIL_SHADOW_MASK_VALUE ) );	// make sure stencil mask passes for the clear
	renderBackend->Clear( false, false, true, 0, 0.0f, 0.0f, 0.0f, 0.0f );	// clear to 0 for stencil select

	// set the depthbounds
	renderBackend->DepthBoundsTest( vLight->scissorRect.zmin, vLight->scissorRect.zmax );


	// two-sided stencil test
	renderBackend->State( GLS_COLORMASK | GLS_ALPHAMASK | GLS_DEPTHMASK | GLS_DEPTHFUNC_LESS | GLS_STENCIL_FUNC_ALWAYS | GLS_STENCIL_MAKE_REF( STENCIL_SHADOW_TEST_VALUE ) | GLS_STENCIL_MAKE_MASK( STENCIL_SHADOW_MASK_VALUE ) | GLS_STENCIL_LIGHT_SELECT );
	renderBackend->Cull( CT_TWO_SIDED );

	renderProgManager.BindShader_Depth();

	// set the matrix for deforming the 'zeroOneCubeModel' into the frustum to exactly cover the light volume
	idRenderMatrix invProjectMVPMatrix;
	idRenderMatrix::Multiply( backEnd.viewDef->worldSpace.mvp, vLight->inverseBaseLightProject, invProjectMVPMatrix );
	RB_SetMVP( invProjectMVPMatrix );

	RB_DrawElementsWithCounters( &backEnd.zeroOneCubeSurface );

	// reset stencil state

	renderBackend->Cull( CT_FRONT_SIDED );

	renderProgManager.Unbind();


	// unset the depthbounds
	renderBackend->DepthBoundsTest( 0.0f, 0.0f );

	renderLog.CloseBlock();
}

/*
==============================================================================================

DRAW INTERACTIONS

==============================================================================================
*/
// Whether the main view traces this light's shadows: it casts some, and its interactions are interaction.vfp's.
static bool RB_LightTracesShadows( const viewLight_t & vLight ) {
	return backEnd.tracedShadows && R_LightTracesShadows( vLight.castsShadows, *vLight.lightShader )
		&& ( vLight.localInteractions != nullptr || vLight.globalInteractions != nullptr );
}

// The traced lights' opaque interactions in one bracket (R16): the scene takes each light whole, the light target
// what a caster of the GPU scene takes of it (R17), and the resolve takes the second sum out of the first.
// False: nothing was drawn, the lights draw with their stencil shadows.
static bool RB_DrawTracedLights( const bool useLightDepthBounds ) {
	bool any = false;
	for ( const viewLight_t * vLight = backEnd.viewDef->viewLights; vLight != nullptr && !any; vLight = vLight->next ) {
		any = RB_LightTracesShadows( *vLight );
	}
	const idScreenRect & v = backEnd.viewDef->viewport;
	const renderRect_t viewport = { v.x1, v.y1, v.GetWidth(), v.GetHeight() };
	if ( !any ) {
		return false;
	}
	if ( !renderBackend->BeginTracedLights( viewport ) ) {
		// the lights draw with what volumes they have, and the front end hears of it (rtShadowLatch_t)
		backEnd.tracedShadows = false;
		return false;
	}

	renderLog.OpenMainBlock( MRB_LIGHT_INTERACTIONS );
	renderLog.OpenBlock( "Traced Lights" );
	// with a history every frame takes other points of the disks (R18); without one each frame the same
	const rtShadowSettings_t & shadow = backEnd.viewDef->rayTracing.shadow;
	tracedLight_t traced = {};
	traced.penumbra = shadow.Penumbrae() ? rtShadowSettings_t::PENUMBRA_SCALE : 0.0f;
	const int sequence = shadow.history ? static_cast<int>( backEnd.viewSequence & 0xFFFFu ) : 0;
	for ( const viewLight_t * vLight = backEnd.viewDef->viewLights; vLight != nullptr; vLight = vLight->next ) {
		if ( !RB_LightTracesShadows( *vLight ) ) {
			continue;
		}
		renderLog.OpenBlock( vLight->lightShader->GetName() );
		if ( useLightDepthBounds ) {
			renderBackend->DepthBoundsTest( vLight->scissorRect.zmin, vLight->scissorRect.zmax );
		}
		// Each light its own points of the disk, by its index in the world: two lights of a pixel share no noise,
		// and a light that enters or leaves the view's list moves no other light's (a float holds the sum exactly).
		traced.noise = sequence + 97 * ( vLight->lightIndex & 0xFFFF );
		// a local surface (MF_NOSELFSHADOW) takes the shadows of the others only, as the stencil order gives it
		traced.mask = gpuSceneInstance_t::MASK_SHADOW_GLOBAL;
		RB_RenderInteractions( vLight->localInteractions, vLight, GLS_DEPTHFUNC_EQUAL, false, useLightDepthBounds, &traced );
		traced.mask = gpuSceneInstance_t::MASK_SHADOW;
		RB_RenderInteractions( vLight->globalInteractions, vLight, GLS_DEPTHFUNC_EQUAL, false, useLightDepthBounds, &traced );
		renderLog.CloseBlock();
	}
	renderLog.CloseBlock();
	renderLog.CloseMainBlock();
	renderBackend->EndTracedLights();

	renderLog.OpenMainBlock( MRB_RT_SHADOWS );
	renderBackend->ResolveTracedShadows( shadow, RB_RayTracingView( *backEnd.viewDef ), viewport );
	renderLog.CloseMainBlock();
	return true;
}

/*
==================
RB_DrawInteractions
==================
*/
static void RB_DrawInteractions() {
	if ( r_skipInteractions.GetBool() ) {
		return;
	}

	renderLog.OpenMainBlock( MRB_DRAW_INTERACTIONS );
	renderLog.OpenBlock( "RB_DrawInteractions" );

	const bool useLightDepthBounds = r_useLightDepthBounds.GetBool();
	const bool traced = backEnd.tracedShadows && RB_DrawTracedLights( useLightDepthBounds );
	// r_rtDebug 6 shows the traced lights alone: nothing else adds to them
	const bool tracedAlone = traced && backEnd.viewDef->rayTracing.shadow.show;

	//
	// for each light, perform shadowing and adding
	//
	for ( const viewLight_t * vLight = tracedAlone ? nullptr : backEnd.viewDef->viewLights; vLight != nullptr; vLight = vLight->next ) {
		// do fogging later
		if ( vLight->lightShader->IsFogLight() ) {
			continue;
		}
		if ( vLight->lightShader->IsBlendLight() ) {
			continue;
		}

		// a traced light has drawn its opaque interactions, shadows in them: its translucent ones are left
		const bool drawn = traced && RB_LightTracesShadows( *vLight );
		const drawSurf_t * const localInteractions = drawn ? nullptr : vLight->localInteractions;
		const drawSurf_t * const globalInteractions = drawn ? nullptr : vLight->globalInteractions;
		const drawSurf_t * const localShadows = drawn ? nullptr : vLight->localShadows;
		const drawSurf_t * const globalShadows = drawn ? nullptr : vLight->globalShadows;

		if ( localInteractions == nullptr && globalInteractions == nullptr && vLight->translucentInteractions == nullptr ) {
			continue;
		}

		const idMaterial * lightShader = vLight->lightShader;
		renderLog.OpenBlock( lightShader->GetName() );

		// set the depth bounds for the whole light
		if ( useLightDepthBounds ) {
			renderBackend->DepthBoundsTest( vLight->scissorRect.zmin, vLight->scissorRect.zmax );
		}

		// only need to clear the stencil buffer and perform stencil testing if there are shadows
		const bool performStencilTest = ( globalShadows != nullptr || localShadows != nullptr );

		// mirror flips the sense of the stencil select, and I don't want to risk accidentally breaking it
		// in the normal case, so simply disable the stencil select in the mirror case
		const bool useLightStencilSelect = ( r_useLightStencilSelect.GetBool() && backEnd.viewDef->isMirror == false );

		if ( performStencilTest ) {
			if ( useLightStencilSelect ) {
				// write a stencil mask for the visible light bounds to hi-stencil
				RB_StencilSelectLight( vLight );
			} else {
				// always clear whole S-Cull tiles
				idScreenRect rect;
				rect.x1 = ( vLight->scissorRect.x1 +  0 ) & ~15;
				rect.y1 = ( vLight->scissorRect.y1 +  0 ) & ~15;
				rect.x2 = ( vLight->scissorRect.x2 + 15 ) & ~15;
				rect.y2 = ( vLight->scissorRect.y2 + 15 ) & ~15;

				if ( !backEnd.currentScissor.Equals( rect ) && r_useScissor.GetBool() ) {
					renderBackend->Scissor( backEnd.viewDef->viewport.x1 + rect.x1,
								backEnd.viewDef->viewport.y1 + rect.y1,
								rect.x2 + 1 - rect.x1,
								rect.y2 + 1 - rect.y1 );
					backEnd.currentScissor = rect;
				}
				renderBackend->State( GLS_DEFAULT );	// make sure stencil mask passes for the clear
				renderBackend->Clear( false, false, true, STENCIL_SHADOW_TEST_VALUE, 0.0f, 0.0f, 0.0f, 0.0f );
			}
		}

		if ( globalShadows != nullptr ) {
			renderLog.OpenMainBlock( MRB_STENCIL_SHADOWS );
			renderLog.OpenBlock( "Global Light Shadows" );
			RB_StencilShadowPass( globalShadows, vLight );
			renderLog.CloseBlock();
			renderLog.CloseMainBlock();
		}

		if ( localInteractions != nullptr ) {
			renderLog.OpenMainBlock( MRB_LIGHT_INTERACTIONS );
			renderLog.OpenBlock( "Local Light Interactions" );
			RB_RenderInteractions( localInteractions, vLight, GLS_DEPTHFUNC_EQUAL, performStencilTest, useLightDepthBounds );
			renderLog.CloseBlock();
			renderLog.CloseMainBlock();
		}

		if ( localShadows != nullptr ) {
			renderLog.OpenMainBlock( MRB_STENCIL_SHADOWS );
			renderLog.OpenBlock( "Local Light Shadows" );
			RB_StencilShadowPass( localShadows, vLight );
			renderLog.CloseBlock();
			renderLog.CloseMainBlock();
		}

		if ( globalInteractions != nullptr ) {
			renderLog.OpenMainBlock( MRB_LIGHT_INTERACTIONS );
			renderLog.OpenBlock( "Global Light Interactions" );
			RB_RenderInteractions( globalInteractions, vLight, GLS_DEPTHFUNC_EQUAL, performStencilTest, useLightDepthBounds );
			renderLog.CloseBlock();
			renderLog.CloseMainBlock();
		}


		if ( vLight->translucentInteractions != nullptr && !r_skipTranslucent.GetBool() ) {
			renderLog.OpenMainBlock( MRB_TRANSLUCENT_INTERACTIONS );
			renderLog.OpenBlock( "Translucent Interactions" );

			// Disable the depth bounds test because translucent surfaces don't work with
			// the depth bounds tests since they did not write depth during the depth pass.
			if ( useLightDepthBounds ) {
				renderBackend->DepthBoundsTest( 0.0f, 0.0f );
			}

			// The depth buffer wasn't filled in for translucent surfaces, so they
			// can never be constrained to perforated surfaces with the depthfunc equal.

			// Translucent surfaces do not receive shadows. This is a case where a
			// shadow buffer solution would work but stencil shadows do not because
			// stencil shadows only affect surfaces that contribute to the view depth
			// buffer and translucent surfaces do not contribute to the view depth buffer.

			RB_RenderInteractions( vLight->translucentInteractions, vLight, GLS_DEPTHFUNC_LESS, false, false );

			renderLog.CloseBlock();
			renderLog.CloseMainBlock();
		}

		renderLog.CloseBlock();
	}

	// disable stencil shadow test
	renderBackend->State( GLS_DEFAULT );

	// reset depth bounds
	if ( useLightDepthBounds ) {
		renderBackend->DepthBoundsTest( 0.0f, 0.0f );
	}

	renderLog.CloseBlock();
	renderLog.CloseMainBlock();
}

/*
=============================================================================================

NON-INTERACTION SHADER PASSES

=============================================================================================
*/

/*
=====================
RB_DrawShaderPasses

Draw non-light dependent passes

If we are rendering Guis, the drawSurf_t::sort value is a depth offset that can
be multiplied by guiEye for polarity and screenSeparation for scale.
=====================
*/
static int RB_DrawShaderPasses( const drawSurf_t * const * const drawSurfs, const int numDrawSurfs, 
									const float guiStereoScreenOffset, const int stereoEye ) {
	// only obey skipAmbient if we are rendering a view
	if ( backEnd.viewDef->viewEntitys && r_skipAmbient.GetBool() ) {
		return numDrawSurfs;
	}

	renderLog.OpenBlock( "RB_DrawShaderPasses" );

	backEnd.currentSpace = (const viewEntity_t *)1;	// using NULL makes /analyze think surf->space needs to be checked...
	float currentGuiStereoOffset = 0.0f;

	int i = 0;
	for ( ; i < numDrawSurfs; i++ ) {
		const drawSurf_t * surf = drawSurfs[i];
		const idMaterial * shader = surf->material;

		if ( !shader->HasAmbient() ) {
			continue;
		}

		if ( shader->IsPortalSky() ) {
			continue;
		}

		// some deforms may disable themselves by setting numIndexes = 0
		if ( surf->numIndexes == 0 ) {
			continue;
		}

		if ( shader->SuppressInSubview() ) {
			continue;
		}

		if ( backEnd.viewDef->isXraySubview && surf->space->entityDef ) {
			if ( surf->space->entityDef->parms.xrayIndex != 2 ) {
				continue;
			}
		}

		// we need to draw the post process shaders after we have drawn the fog lights
		if ( shader->GetSort() >= static_cast< float >( SS_POST_PROCESS ) && !backEnd.currentRenderCopied ) {
			break;
		}

		// if we are rendering a 3D view and the surface's eye index doesn't match 
		// the current view's eye index then we skip the surface
		// if the stereoEye value of a surface is 0 then we need to draw it for both eyes.
		const int shaderStereoEye = shader->GetStereoEye();
		const bool isEyeValid = stereoRender_swapEyes.GetBool() ? ( shaderStereoEye == stereoEye ) : ( shaderStereoEye != stereoEye );
		if ( ( stereoEye != 0 ) && ( shaderStereoEye != 0 ) && ( isEyeValid ) ) {
			continue;
		}

		renderLog.OpenBlock( shader->GetName() );

		// determine the stereoDepth offset 
		// guiStereoScreenOffset will always be zero for 3D views, so the !=
		// check will never force an update due to the current sort value.
		const float thisGuiStereoOffset = guiStereoScreenOffset * surf->sort;

		// change the matrix and other space related vars if needed
		if ( surf->space != backEnd.currentSpace || thisGuiStereoOffset != currentGuiStereoOffset ) {
			backEnd.currentSpace = surf->space;
			currentGuiStereoOffset = thisGuiStereoOffset;

			const viewEntity_t *space = backEnd.currentSpace;

			if ( guiStereoScreenOffset != 0.0f ) {
				RB_SetMVPWithStereoOffset( space->mvp, currentGuiStereoOffset );
			} else {
				RB_SetMVP( space->mvp );
			}

			// set eye position in local space
			idVec4 localViewOrigin( 1.0f );
			R_GlobalPointToLocal( space->modelMatrix, backEnd.viewDef->renderView.vieworg, localViewOrigin.ToVec3() );
			SetVertexParm( RENDERPARM_LOCALVIEWORIGIN, localViewOrigin.ToFloatPtr() );

			// set model Matrix
			float modelMatrixTranspose[16];
			R_MatrixTranspose( space->modelMatrix, modelMatrixTranspose );
			SetVertexParms( RENDERPARM_MODELMATRIX_X, modelMatrixTranspose, 4 );

			// Set ModelView Matrix
			float modelViewMatrixTranspose[16];
			R_MatrixTranspose( space->modelViewMatrix, modelViewMatrixTranspose );
			SetVertexParms( RENDERPARM_MODELVIEWMATRIX_X, modelViewMatrixTranspose, 4 );
		}

		// change the scissor if needed
		if ( !backEnd.currentScissor.Equals( surf->scissorRect ) && r_useScissor.GetBool() ) {
			renderBackend->Scissor( backEnd.viewDef->viewport.x1 + surf->scissorRect.x1, 
						backEnd.viewDef->viewport.y1 + surf->scissorRect.y1,
						surf->scissorRect.x2 + 1 - surf->scissorRect.x1,
						surf->scissorRect.y2 + 1 - surf->scissorRect.y1 );
			backEnd.currentScissor = surf->scissorRect;
		}

		// get the expressions for conditionals / color / texcoords
		const float	*regs = surf->shaderRegisters;

		// set face culling appropriately
		if ( surf->space->isGuiSurface ) {
			renderBackend->Cull( CT_TWO_SIDED );
		} else {
			renderBackend->Cull( shader->GetCullType() );
		}

		uint64 surfGLState = surf->extraGLState;

		// set polygon offset if necessary
		if ( shader->TestMaterialFlag(MF_POLYGONOFFSET) ) {
			renderBackend->PolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset() );
			surfGLState = GLS_POLYGON_OFFSET;
		}

		for ( int stage = 0; stage < shader->GetNumStages(); stage++ ) {		
			const shaderStage_t *pStage = shader->GetStage(stage);

			// check the enable condition
			if ( regs[ pStage->conditionRegister ] == 0 ) {
				continue;
			}

			// skip the stages involved in lighting
			if ( pStage->lighting != SL_AMBIENT ) {
				continue;
			}

			uint64 stageGLState = surfGLState;
			if ( ( surfGLState & GLS_OVERRIDE ) == 0 ) {
				stageGLState |= pStage->drawStateBits;
			}

			// skip if the stage is ( GL_ZERO, GL_ONE ), which is used for some alpha masks
			if ( ( stageGLState & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) == ( GLS_SRCBLEND_ZERO | GLS_DSTBLEND_ONE ) ) {
				continue;
			}

			// see if we are a new-style stage
			newShaderStage_t *newStage = pStage->newStage;
			if ( newStage != NULL ) {
				//--------------------------
				//
				// new style stages
				//
				//--------------------------
				if ( r_skipNewAmbient.GetBool() ) {
					continue;
				}
				renderLog.OpenBlock( "New Shader Stage" );

				renderBackend->State( stageGLState );
			
				renderProgManager.BindShader( newStage->glslProgram, newStage->glslProgram );

				for ( int j = 0; j < newStage->numVertexParms; j++ ) {
					float parm[4];
					parm[0] = regs[ newStage->vertexParms[j][0] ];
					parm[1] = regs[ newStage->vertexParms[j][1] ];
					parm[2] = regs[ newStage->vertexParms[j][2] ];
					parm[3] = regs[ newStage->vertexParms[j][3] ];
					SetVertexParm( (renderParm_t)( RENDERPARM_USER + j ), parm );
				}

				// set rpEnableSkinning if the shader has optional support for skinning
				if ( surf->jointCache && renderProgManager.ShaderHasOptionalSkinning() ) {
					const idVec4 skinningParm( 1.0f );
					SetVertexParm( RENDERPARM_ENABLE_SKINNING, skinningParm.ToFloatPtr() );
				}

				// bind texture units
				for ( int j = 0; j < newStage->numFragmentProgramImages; j++ ) {
					idImage * image = newStage->fragmentProgramImages[j];
					if ( image != NULL ) {
						image->Bind( j );
					}
				}

				// draw it
				RB_DrawElementsWithCounters( surf );

				// clear rpEnableSkinning if it was set
				if ( surf->jointCache && renderProgManager.ShaderHasOptionalSkinning() ) {
					const idVec4 skinningParm( 0.0f );
					SetVertexParm( RENDERPARM_ENABLE_SKINNING, skinningParm.ToFloatPtr() );
				}

				renderProgManager.Unbind();

				renderLog.CloseBlock();
				continue;
			}

			//--------------------------
			//
			// old style stages
			//
			//--------------------------

			// set the color
			float color[4];
			color[0] = regs[ pStage->color.registers[0] ];
			color[1] = regs[ pStage->color.registers[1] ];
			color[2] = regs[ pStage->color.registers[2] ];
			color[3] = regs[ pStage->color.registers[3] ];

			// skip the entire stage if an add would be black
			if ( ( stageGLState & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) == ( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE ) 
				&& color[0] <= 0 && color[1] <= 0 && color[2] <= 0 ) {
				continue;
			}

			// skip the entire stage if a blend would be completely transparent
			if ( ( stageGLState & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) == ( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA )
				&& color[3] <= 0 ) {
				continue;
			}

			stageVertexColor_t svc = pStage->vertexColor;

			renderLog.OpenBlock( "Old Shader Stage" );
			RB_SetColor( color );

			if ( surf->space->isGuiSurface ) {
				// Force gui surfaces to always be SVC_MODULATE
				svc = SVC_MODULATE;

				// use special shaders for bink cinematics
				if ( pStage->texture.cinematic ) {
					if ( ( stageGLState & GLS_OVERRIDE ) != 0 ) {
						// This is a hack... Only SWF Guis set GLS_OVERRIDE
						// Old style guis do not, and we don't want them to use the new GUI renederProg
						renderProgManager.BindShader_BinkGUI();
					} else {
						renderProgManager.BindShader_Bink();
					}
				} else {
					if ( ( stageGLState & GLS_OVERRIDE ) != 0 ) {
						// This is a hack... Only SWF Guis set GLS_OVERRIDE
						// Old style guis do not, and we don't want them to use the new GUI renderProg
						renderProgManager.BindShader_GUI();
					} else {
						if ( surf->jointCache ) {
							renderProgManager.BindShader_TextureVertexColorSkinned();
						} else {
							renderProgManager.BindShader_TextureVertexColor();
						}
					}
				}
			} else if ( ( pStage->texture.texgen == TG_SCREEN ) || ( pStage->texture.texgen == TG_SCREEN2 ) ) {
				renderProgManager.BindShader_TextureTexGenVertexColor();
			} else if ( pStage->texture.cinematic ) {
				renderProgManager.BindShader_Bink();
			} else {
				if ( surf->jointCache ) {
					renderProgManager.BindShader_TextureVertexColorSkinned();
				} else {
					renderProgManager.BindShader_TextureVertexColor();
				}
			}
		
			RB_SetVertexColorParms( svc );

			// bind the texture
			RB_BindVariableStageImage( &pStage->texture, regs );

			// set privatePolygonOffset if necessary
			if ( pStage->privatePolygonOffset ) {
				renderBackend->PolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * pStage->privatePolygonOffset );
				stageGLState |= GLS_POLYGON_OFFSET;
			}

			// set the state
			renderBackend->State( stageGLState );
		
			RB_PrepareStageTexturing( pStage, surf );

			// draw it
			RB_DrawElementsWithCounters( surf );

			RB_FinishStageTexturing( pStage, surf );

			// unset privatePolygonOffset if necessary
			if ( pStage->privatePolygonOffset ) {
				renderBackend->PolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset() );
			}
			renderLog.CloseBlock();
		}

		renderLog.CloseBlock();
	}

	renderBackend->Cull( CT_FRONT_SIDED );
	RB_SetColor( 1.0f, 1.0f, 1.0f );

	renderLog.CloseBlock();
	return i;
}

/*
=============================================================================================

BLEND LIGHT PROJECTION

=============================================================================================
*/

/*
=====================
RB_T_BlendLight
=====================
*/
static void RB_T_BlendLight( const drawSurf_t *drawSurfs, const viewLight_t * vLight ) {
	backEnd.currentSpace = NULL;

	for ( const drawSurf_t * drawSurf = drawSurfs; drawSurf != NULL; drawSurf = drawSurf->nextOnLight ) {
		if ( drawSurf->scissorRect.IsEmpty() ) {
			continue;	// !@# FIXME: find out why this is sometimes being hit!
						// temporarily jump over the scissor and draw so the gl error callback doesn't get hit
		}

		if ( !backEnd.currentScissor.Equals( drawSurf->scissorRect ) && r_useScissor.GetBool() ) {
			// change the scissor
			renderBackend->Scissor( backEnd.viewDef->viewport.x1 + drawSurf->scissorRect.x1,
						backEnd.viewDef->viewport.y1 + drawSurf->scissorRect.y1,
						drawSurf->scissorRect.x2 + 1 - drawSurf->scissorRect.x1,
						drawSurf->scissorRect.y2 + 1 - drawSurf->scissorRect.y1 );
			backEnd.currentScissor = drawSurf->scissorRect;
		}

		if ( drawSurf->space != backEnd.currentSpace ) {
			// change the matrix
			RB_SetMVP( drawSurf->space->mvp );

			// change the light projection matrix
			idPlane	lightProjectInCurrentSpace[4];
			for ( int i = 0; i < 4; i++ ) {
				R_GlobalPlaneToLocal( drawSurf->space->modelMatrix, vLight->lightProject[i], lightProjectInCurrentSpace[i] );
			}

			SetVertexParm( RENDERPARM_TEXGEN_0_S, lightProjectInCurrentSpace[0].ToFloatPtr() );
			SetVertexParm( RENDERPARM_TEXGEN_0_T, lightProjectInCurrentSpace[1].ToFloatPtr() );
			SetVertexParm( RENDERPARM_TEXGEN_0_Q, lightProjectInCurrentSpace[2].ToFloatPtr() );
			SetVertexParm( RENDERPARM_TEXGEN_1_S, lightProjectInCurrentSpace[3].ToFloatPtr() );	// falloff

			backEnd.currentSpace = drawSurf->space;
		}

		RB_DrawElementsWithCounters( drawSurf );
	}
}

/*
=====================
RB_BlendLight

Dual texture together the falloff and projection texture with a blend
mode to the framebuffer, instead of interacting with the surface texture
=====================
*/
static void RB_BlendLight( const drawSurf_t *drawSurfs, const drawSurf_t *drawSurfs2, const viewLight_t * vLight ) {
	if ( drawSurfs == NULL ) {
		return;
	}
	if ( r_skipBlendLights.GetBool() ) {
		return;
	}
	renderLog.OpenBlock( vLight->lightShader->GetName() );

	const idMaterial * lightShader = vLight->lightShader;
	const float	* regs = vLight->shaderRegisters;

	// texture 1 will get the falloff texture
	vLight->falloffImage->Bind( 1 );

	renderProgManager.BindShader_BlendLight();

	for ( int i = 0; i < lightShader->GetNumStages(); i++ ) {
		const shaderStage_t	*stage = lightShader->GetStage(i);

		if ( !regs[ stage->conditionRegister ] ) {
			continue;
		}

		renderBackend->State( GLS_DEPTHMASK | stage->drawStateBits | GLS_DEPTHFUNC_EQUAL );

		// texture 0 will get the projected texture
		stage->texture.image->Bind( 0 );

		if ( stage->texture.hasMatrix ) {
			RB_LoadShaderTextureMatrix( regs, &stage->texture );
		}

		// get the modulate values from the light, including alpha, unlike normal lights
		float lightColor[4];
		lightColor[0] = regs[ stage->color.registers[0] ];
		lightColor[1] = regs[ stage->color.registers[1] ];
		lightColor[2] = regs[ stage->color.registers[2] ];
		lightColor[3] = regs[ stage->color.registers[3] ];
		RB_SetColor( lightColor );

		RB_T_BlendLight( drawSurfs, vLight );
		RB_T_BlendLight( drawSurfs2, vLight );
	}

	renderProgManager.Unbind();
	renderLog.CloseBlock();
}

/*
=========================================================================================================

FOG LIGHTS

=========================================================================================================
*/

/*
=====================
RB_T_BasicFog
=====================
*/
static void RB_T_BasicFog( const drawSurf_t *drawSurfs, const idPlane fogPlanes[4], const idRenderMatrix * inverseBaseLightProject ) {
	backEnd.currentSpace = NULL;

	for ( const drawSurf_t * drawSurf = drawSurfs; drawSurf != NULL; drawSurf = drawSurf->nextOnLight ) {
		if ( drawSurf->scissorRect.IsEmpty() ) {
			continue;	// !@# FIXME: find out why this is sometimes being hit!
						// temporarily jump over the scissor and draw so the gl error callback doesn't get hit
		}

		if ( !backEnd.currentScissor.Equals( drawSurf->scissorRect ) && r_useScissor.GetBool() ) {
			// change the scissor
			renderBackend->Scissor( backEnd.viewDef->viewport.x1 + drawSurf->scissorRect.x1,
						backEnd.viewDef->viewport.y1 + drawSurf->scissorRect.y1,
						drawSurf->scissorRect.x2 + 1 - drawSurf->scissorRect.x1,
						drawSurf->scissorRect.y2 + 1 - drawSurf->scissorRect.y1 );
			backEnd.currentScissor = drawSurf->scissorRect;
		}

		if ( drawSurf->space != backEnd.currentSpace ) {
			idPlane localFogPlanes[4];
			if ( inverseBaseLightProject == NULL ) {
				RB_SetMVP( drawSurf->space->mvp );
				for ( int i = 0; i < 4; i++ ) {
					R_GlobalPlaneToLocal( drawSurf->space->modelMatrix, fogPlanes[i], localFogPlanes[i] );
				}
			} else {
				idRenderMatrix invProjectMVPMatrix;
				idRenderMatrix::Multiply( backEnd.viewDef->worldSpace.mvp, *inverseBaseLightProject, invProjectMVPMatrix );
				RB_SetMVP( invProjectMVPMatrix );
				for ( int i = 0; i < 4; i++ ) {
					inverseBaseLightProject->InverseTransformPlane( fogPlanes[i], localFogPlanes[i], false );
				}
			}

			SetVertexParm( RENDERPARM_TEXGEN_0_S, localFogPlanes[0].ToFloatPtr() );
			SetVertexParm( RENDERPARM_TEXGEN_0_T, localFogPlanes[1].ToFloatPtr() );
			SetVertexParm( RENDERPARM_TEXGEN_1_T, localFogPlanes[2].ToFloatPtr() );
			SetVertexParm( RENDERPARM_TEXGEN_1_S, localFogPlanes[3].ToFloatPtr() );

			backEnd.currentSpace = ( inverseBaseLightProject == NULL ) ? drawSurf->space : NULL;
		}

		if ( drawSurf->jointCache ) {
			renderProgManager.BindShader_FogSkinned();
		} else {
			renderProgManager.BindShader_Fog();
		}

		RB_DrawElementsWithCounters( drawSurf );
	}
}

/*
==================
RB_FogPass
==================
*/
static void RB_FogPass( const drawSurf_t * drawSurfs,  const drawSurf_t * drawSurfs2, const viewLight_t * vLight ) {
	renderLog.OpenBlock( vLight->lightShader->GetName() );

	// find the current color and density of the fog
	const idMaterial * lightShader = vLight->lightShader;
	const float * regs = vLight->shaderRegisters;
	// assume fog shaders have only a single stage
	const shaderStage_t * stage = lightShader->GetStage( 0 );

	float lightColor[4];
	lightColor[0] = regs[ stage->color.registers[0] ];
	lightColor[1] = regs[ stage->color.registers[1] ];
	lightColor[2] = regs[ stage->color.registers[2] ];
	lightColor[3] = regs[ stage->color.registers[3] ];

	RB_SetColor( lightColor );

	// calculate the falloff planes
	float a;

	// if they left the default value on, set a fog distance of 500
	if ( lightColor[3] <= 1.0f ) {
		a = -0.5f / DEFAULT_FOG_DISTANCE;
	} else {
		// otherwise, distance = alpha color
		a = -0.5f / lightColor[3];
	}

	// texture 0 is the falloff image
	globalImages->fogImage->Bind( 0 );

	// texture 1 is the entering plane fade correction
	globalImages->fogEnterImage->Bind( 1 );

	// S is based on the view origin
	const float s = vLight->fogPlane.Distance( backEnd.viewDef->renderView.vieworg );

	const float FOG_SCALE = 0.001f;

	idPlane fogPlanes[4];

	// S-0
	fogPlanes[0][0] = a * backEnd.viewDef->worldSpace.modelViewMatrix[0*4+2];
	fogPlanes[0][1] = a * backEnd.viewDef->worldSpace.modelViewMatrix[1*4+2];
	fogPlanes[0][2] = a * backEnd.viewDef->worldSpace.modelViewMatrix[2*4+2];
	fogPlanes[0][3] = a * backEnd.viewDef->worldSpace.modelViewMatrix[3*4+2] + 0.5f;

	// T-0
	fogPlanes[1][0] = 0.0f;//a * backEnd.viewDef->worldSpace.modelViewMatrix[0*4+0];
	fogPlanes[1][1] = 0.0f;//a * backEnd.viewDef->worldSpace.modelViewMatrix[1*4+0];
	fogPlanes[1][2] = 0.0f;//a * backEnd.viewDef->worldSpace.modelViewMatrix[2*4+0];
	fogPlanes[1][3] = 0.5f;//a * backEnd.viewDef->worldSpace.modelViewMatrix[3*4+0] + 0.5f;

	// T-1 will get a texgen for the fade plane, which is always the "top" plane on unrotated lights
	fogPlanes[2][0] = FOG_SCALE * vLight->fogPlane[0];
	fogPlanes[2][1] = FOG_SCALE * vLight->fogPlane[1];
	fogPlanes[2][2] = FOG_SCALE * vLight->fogPlane[2];
	fogPlanes[2][3] = FOG_SCALE * vLight->fogPlane[3] + FOG_ENTER;

	// S-1
	fogPlanes[3][0] = 0.0f;
	fogPlanes[3][1] = 0.0f;
	fogPlanes[3][2] = 0.0f;
	fogPlanes[3][3] = FOG_SCALE * s + FOG_ENTER;

	// draw it
	renderBackend->State( GLS_DEPTHMASK | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHFUNC_EQUAL );
	RB_T_BasicFog( drawSurfs, fogPlanes, NULL );
	RB_T_BasicFog( drawSurfs2, fogPlanes, NULL );

	// the light frustum bounding planes aren't in the depth buffer, so use depthfunc_less instead
	// of depthfunc_equal
	renderBackend->State( GLS_DEPTHMASK | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHFUNC_LESS );
	renderBackend->Cull( CT_BACK_SIDED );

	backEnd.zeroOneCubeSurface.space = &backEnd.viewDef->worldSpace;
	backEnd.zeroOneCubeSurface.scissorRect = backEnd.viewDef->scissor;
	RB_T_BasicFog( &backEnd.zeroOneCubeSurface, fogPlanes, &vLight->inverseBaseLightProject );

	renderBackend->Cull( CT_FRONT_SIDED );

	renderProgManager.Unbind();

	renderLog.CloseBlock();
}

/*
==================
RB_FogAllLights
==================
*/
static void RB_FogAllLights() {
	if ( r_skipFogLights.GetBool()
		 || backEnd.viewDef->isXraySubview /* don't fog in xray mode*/ ) {
		return;
	}
	renderLog.OpenMainBlock( MRB_FOG_ALL_LIGHTS );
	renderLog.OpenBlock( "RB_FogAllLights" );

	// force fog plane to recalculate
	backEnd.currentSpace = NULL;

	for ( viewLight_t * vLight = backEnd.viewDef->viewLights; vLight != NULL; vLight = vLight->next ) {
		if ( vLight->lightShader->IsFogLight() ) {
			RB_FogPass( vLight->globalInteractions, vLight->localInteractions, vLight );
		} else if ( vLight->lightShader->IsBlendLight() ) {
			RB_BlendLight( vLight->globalInteractions, vLight->localInteractions, vLight );
		}
	}

	renderLog.CloseBlock();
	renderLog.CloseMainBlock();
}

/*
=========================================================================================================

BACKEND COMMANDS

=========================================================================================================
*/

/*
==================
RB_DrawViewInternal
==================
*/
// The gamma composite of the view (R13): the backend converts the view's rectangle of the HDR scene.
static void RB_SetHdrGamma( bool gamma ) {
	if ( !backEnd.hdrPhase || backEnd.hdrGamma == gamma ) {
		return;
	}
	const idScreenRect & v = backEnd.viewDef->viewport;
	renderBackend->SetHdrSceneGamma( gamma, { v.x1, v.y1, v.GetWidth(), v.GetHeight() } );
	backEnd.hdrGamma = gamma;
}

void RB_DrawViewInternal( const viewDef_t * viewDef, const int stereoEye, bool debugTools ) {
	renderLog.OpenBlock( "RB_DrawViewInternal" );

	//-------------------------------------------------
	// guis can wind up referencing purged images that need to be loaded.
	// this used to be in the gui emit code, but now that it can be running
	// in a separate thread, it must not try to load images, so do it here.
	//-------------------------------------------------
	drawSurf_t **drawSurfs = (drawSurf_t **)&viewDef->drawSurfs[0];
	const int numDrawSurfs = viewDef->numDrawSurfs;

	for ( int i = 0; i < numDrawSurfs; i++ ) {
		const drawSurf_t * ds = viewDef->drawSurfs[ i ];
		if ( ds->material != NULL ) {
			const_cast<idMaterial *>( ds->material )->EnsureNotPurged();
		}
	}

	//-------------------------------------------------
	// RB_BeginDrawingView
	//
	// Any mirrored or portaled views have already been drawn, so prepare
	// to actually render the visible surfaces for this view
	//
	// clear the z buffer, set the projection matrix, etc
	//-------------------------------------------------
	renderLog.OpenMainBlock( MRB_BEGIN_DRAWING_VIEW );

	// set the window clipping
	renderBackend->Viewport( viewDef->viewport.x1,
		viewDef->viewport.y1,
		viewDef->viewport.x2 + 1 - viewDef->viewport.x1,
		viewDef->viewport.y2 + 1 - viewDef->viewport.y1 );

	// the scissor may be smaller than the viewport for subviews
	renderBackend->Scissor( backEnd.viewDef->viewport.x1 + viewDef->scissor.x1,
				backEnd.viewDef->viewport.y1 + viewDef->scissor.y1,
				viewDef->scissor.x2 + 1 - viewDef->scissor.x1,
				viewDef->scissor.y2 + 1 - viewDef->scissor.y1 );
	backEnd.currentScissor = viewDef->scissor;

	// force face culling to set next time; the backend also starts its view here
	renderBackend->SetViewMirror( backEnd.viewDef->isMirror );

	// ensures that depth writes are enabled for the depth clear
	renderBackend->State( GLS_DEFAULT );


	// Clear the depth buffer and clear the stencil to 128 for stencil shadows as well as gui masking
	renderBackend->Clear( false, true, true, STENCIL_SHADOW_TEST_VALUE, 0.0f, 0.0f, 0.0f, 0.0f );

	// normal face culling
	renderBackend->Cull( CT_FRONT_SIDED );

	renderLog.CloseMainBlock();

	//------------------------------------
	// sets variables that can be used by all programs
	//------------------------------------
	{
		//
		// set eye position in global space
		//
		float parm[4];
		parm[0] = backEnd.viewDef->renderView.vieworg[0];
		parm[1] = backEnd.viewDef->renderView.vieworg[1];
		parm[2] = backEnd.viewDef->renderView.vieworg[2];
		parm[3] = 1.0f;

		SetVertexParm( RENDERPARM_GLOBALEYEPOS, parm ); // rpGlobalEyePos

		// sets overbright to make world brighter
		// This value is baked into the specularScale and diffuseScale values so
		// the interaction programs don't need to perform the extra multiply,
		// but any other renderprogs that want to obey the brightness value
		// can reference this.
		float overbright = r_lightScale.GetFloat() * 0.5f;
		parm[0] = overbright;
		parm[1] = overbright;
		parm[2] = overbright;
		parm[3] = overbright;
		SetFragmentParm( RENDERPARM_OVERBRIGHT, parm );

		// Set Projection Matrix
		float projMatrixTranspose[16];
		R_MatrixTranspose( backEnd.viewDef->projectionMatrix, projMatrixTranspose );
		SetVertexParms( RENDERPARM_PROJMATRIX_X, projMatrixTranspose, 4 );
	}

	//-------------------------------------------------
	// fill the depth buffer and clear color buffer to black except on subviews
	//-------------------------------------------------
	RB_FillDepthBufferFast( drawSurfs, numDrawSurfs );
	if ( backEnd.gbufferView && viewDef->viewEntitys != nullptr && numDrawSurfs > 0 ) {
		RB_FillGBuffer( drawSurfs, numDrawSurfs );
	}

	//-------------------------------------------------
	// main light renderer
	//-------------------------------------------------
	RB_DrawInteractions();

	// R20, R19: the reflections and the indirect light add to the lights in linear, before the passes that
	// blend in gamma
	const rayTracingSettings_t & rayTracing = viewDef->rayTracing;
	if ( backEnd.gbufferView && viewDef->gpuScene != nullptr && ( rayTracing.reflection.enabled || rayTracing.indirect.enabled ) ) {
		const idScreenRect & v = viewDef->viewport;
		const renderRect_t viewport = { v.x1, v.y1, v.GetWidth(), v.GetHeight() };
		if ( rayTracing.reflection.enabled ) {
			renderLog.OpenMainBlock( MRB_RT_REFLECTIONS );
			renderBackend->DrawRayTracedReflections( rayTracing.reflection, RB_RayTracingView( *viewDef ), viewport );
			renderLog.CloseMainBlock();
		}
		if ( rayTracing.indirect.enabled ) {
			renderLog.OpenMainBlock( MRB_RT_INDIRECT );
			renderBackend->DrawRayTracedIndirect( rayTracing.indirect, RB_RayTracingView( *viewDef ), viewport );
			renderLog.CloseMainBlock();
		}
	}

	// in HDR the lights add in linear, what follows blends in gamma as in the retail frame
	RB_SetHdrGamma( r_hdrGammaBlend.GetBool() );

	//-------------------------------------------------
	// now draw any non-light dependent shading passes
	//-------------------------------------------------
	int processed = 0;
	if ( !r_skipShaderPasses.GetBool() ) {
		// 2D views (menus, HUD, console) only have shader passes, r_pix shows them apart
		renderLog.OpenMainBlock( viewDef->viewEntitys != NULL ? MRB_DRAW_SHADER_PASSES : MRB_DRAW_GUI );
		float guiScreenOffset;
		if ( viewDef->viewEntitys != NULL ) {
			// guiScreenOffset will be 0 in non-gui views
			guiScreenOffset = 0.0f;
		} else {
			guiScreenOffset = stereoEye * viewDef->renderView.stereoScreenSeparation;
		}
		processed = RB_DrawShaderPasses( drawSurfs, numDrawSurfs, guiScreenOffset, stereoEye );
		renderLog.CloseMainBlock();
	}

	//-------------------------------------------------
	// fog and blend lights, drawn after emissive surfaces
	// so they are properly dimmed down
	//-------------------------------------------------
	RB_FogAllLights();
	RB_SetHdrGamma( false );

	//-------------------------------------------------
	// capture the depth for the motion blur before rendering any post process surfaces that may contribute to the depth
	//-------------------------------------------------
	if ( r_motionBlur.GetInteger() > 0 ) {
		renderLog.OpenMainBlock( MRB_CAPTURE_COLORBUFFER );
		const idScreenRect & viewport = backEnd.viewDef->viewport;
		globalImages->currentDepthImage->CopyDepthbuffer( viewport.x1, viewport.y1, viewport.GetWidth(), viewport.GetHeight() );
		renderLog.CloseMainBlock();
	}

	//-------------------------------------------------
	// now draw any screen warping post-process effects using _currentRender
	//-------------------------------------------------
	if ( processed < numDrawSurfs && !r_skipPostProcess.GetBool() ) {
		int x = backEnd.viewDef->viewport.x1;
		int y = backEnd.viewDef->viewport.y1;
		int	w = backEnd.viewDef->viewport.x2 - backEnd.viewDef->viewport.x1 + 1;
		int	h = backEnd.viewDef->viewport.y2 - backEnd.viewDef->viewport.y1 + 1;

		RENDERLOG_PRINTF( "Resolve to %i x %i buffer\n", w, h );

		// resolve the screen
		renderLog.OpenMainBlock( MRB_CAPTURE_COLORBUFFER );
		globalImages->currentRenderImage->CopyFramebuffer( x, y, w, h );
		renderLog.CloseMainBlock();
		backEnd.currentRenderCopied = true;

		// RENDERPARM_SCREENCORRECTIONFACTOR amd RENDERPARM_WINDOWCOORD overlap
		// diffuseScale and specularScale

		// screen power of two correction factor (no longer relevant now)
		float screenCorrectionParm[4];
		screenCorrectionParm[0] = 1.0f;
		screenCorrectionParm[1] = 1.0f;
		screenCorrectionParm[2] = 0.0f;
		screenCorrectionParm[3] = 1.0f;
		SetFragmentParm( RENDERPARM_SCREENCORRECTIONFACTOR, screenCorrectionParm ); // rpScreenCorrectionFactor

		// window coord to 0.0 to 1.0 conversion
		float windowCoordParm[4];
		windowCoordParm[0] = 1.0f / w;
		windowCoordParm[1] = 1.0f / h;
		windowCoordParm[2] = 0.0f;
		windowCoordParm[3] = 1.0f;
		SetFragmentParm( RENDERPARM_WINDOWCOORD, windowCoordParm ); // rpWindowCoord

		// render the remaining surfaces
		renderLog.OpenMainBlock( MRB_DRAW_SHADER_PASSES_POST );
		RB_DrawShaderPasses( drawSurfs + processed, numDrawSurfs - processed, 0.0f /* definitely not a gui */, stereoEye );
		renderLog.CloseMainBlock();
	}

	//-------------------------------------------------
	// render debug tools; the HDR main view draws them after its resolve, over the tonemapped frame
	//-------------------------------------------------
	if ( debugTools ) {
		RB_RenderDebugTools( drawSurfs, numDrawSurfs );
	}

	renderLog.CloseBlock();
}

/*
==================
RB_MotionBlur

Experimental feature
==================
*/
void RB_MotionBlur() {
	if ( !backEnd.viewDef->viewEntitys ) {
		// 3D views only
		return;
	}
	if ( r_motionBlur.GetInteger() <= 0 ) {
		return;
	}
	if ( backEnd.viewDef->isSubview ) {
		return;
	}

	renderLog.OpenMainBlock( MRB_POSTPROCESS );
	renderLog.OpenBlock( "RB_MotionBlur" );

	renderBackend->CheckErrors();

	// clear the alpha buffer and draw only the hands + weapon into it so
	// we can avoid blurring them
	renderBackend->State( GLS_COLORMASK | GLS_DEPTHMASK );
	renderBackend->Clear( true, false, false, 0, 0, 0, 0, 1 );		// only the alpha: GLS_COLORMASK
	RB_SetColor( 0, 0, 0, 0 );
	globalImages->blackImage->Bind( 0 );
	backEnd.currentSpace = NULL;

	drawSurf_t **drawSurfs = (drawSurf_t **)&backEnd.viewDef->drawSurfs[0];
	for ( int surfNum = 0; surfNum < backEnd.viewDef->numDrawSurfs; surfNum++ ) {
		const drawSurf_t * surf = drawSurfs[ surfNum ];

		if ( !surf->space->weaponDepthHack && !surf->space->skipMotionBlur && !surf->material->HasSubview() ) {
			// Apply motion blur to this object
			continue;
		}

		const idMaterial * shader = surf->material;
		if ( shader->Coverage() == MC_TRANSLUCENT ) {
			// muzzle flash, etc
			continue;
		}

		// set mvp matrix
		if ( surf->space != backEnd.currentSpace ) {
			RB_SetMVP( surf->space->mvp );
			backEnd.currentSpace = surf->space;
		}

		// this could just be a color, but we don't have a skinned color-only prog
		if ( surf->jointCache ) {
			renderProgManager.BindShader_TextureVertexColorSkinned();
		} else {
			renderProgManager.BindShader_TextureVertexColor();
		}

		// draw it solid
		RB_DrawElementsWithCounters( surf );
	}
	renderBackend->State( GLS_DEPTHFUNC_ALWAYS );

	// copy off the color buffer and the depth buffer for the motion blur prog
	// we use the viewport dimensions for copying the buffers in case resolution scaling is enabled.
	const idScreenRect & viewport = backEnd.viewDef->viewport;
	globalImages->currentRenderImage->CopyFramebuffer( viewport.x1, viewport.y1, viewport.GetWidth(), viewport.GetHeight() );

	// in stereo rendering, each eye needs to get a separate previous frame mvp
	int mvpIndex = ( backEnd.viewDef->renderView.viewEyeBuffer == 1 ) ? 1 : 0;

	// derive the matrix to go from current pixels to previous frame pixels
	idRenderMatrix	inverseMVP;
	idRenderMatrix::Inverse( backEnd.viewDef->worldSpace.mvp, inverseMVP );

	idRenderMatrix	motionMatrix;
	idRenderMatrix::Multiply( backEnd.prevMVP[mvpIndex], inverseMVP, motionMatrix );

	backEnd.prevMVP[mvpIndex] = backEnd.viewDef->worldSpace.mvp;

	RB_SetMVP( motionMatrix );

	renderBackend->State( GLS_DEPTHFUNC_ALWAYS );
	renderBackend->Cull( CT_TWO_SIDED );

	renderProgManager.BindShader_MotionBlur();

	// let the fragment program know how many samples we are going to use
	idVec4 samples( (float)( 1 << r_motionBlur.GetInteger() ) );
	SetFragmentParm( RENDERPARM_OVERBRIGHT, samples.ToFloatPtr() );

	globalImages->currentRenderImage->Bind( 0 );
	globalImages->currentDepthImage->Bind( 1 );

	RB_DrawElementsWithCounters( &backEnd.unitSquareSurface );
	renderBackend->CheckErrors();

	renderLog.CloseBlock();
	renderLog.CloseMainBlock();
}

/*
==================
RB_DrawView

StereoEye will always be 0 in mono modes, or -1 / 1 in stereo modes.
If the view is a GUI view that is repeated for both eyes, the viewDef.stereoEye value
is 0, so the stereoEye parameter is not always the same as that.
==================
*/
bool RB_ViewDraws( const viewDef_t * viewDef ) {
	// skip render bypasses everything that has models, assuming
	// them to be 3D views, but leaves 2D rendering visible
	return viewDef->numDrawSurfs != 0 && !( r_skipRender.GetBool() && viewDef->viewEntitys );
}

// The eye adaptation's step at the main view's resolve: its time since the last resolve; at once to the target
// for a new view, viewport size or a time that went back; none while a screenshot is taken or for the second eye.
static hdrExposureStep_t RB_HdrExposureStep( const viewDef_t & viewDef, int stereoEye ) {
	hdrExposureStep_t step;
	if ( tr.takingScreenshot || stereoEye == -1 ) {
		step.reset = false;
		return step;
	}
	hdrLastResolve_t & last = backEnd.hdrLastResolve;
	const int time = viewDef.renderView.time[0];
	const int width = viewDef.viewport.GetWidth();
	const int height = viewDef.viewport.GetHeight();
	step.reset = !last.valid || viewDef.renderView.viewID != last.viewID || width != last.width || height != last.height || time < last.time;
	step.seconds = step.reset ? 0.0f : static_cast<float>( time - last.time ) * 0.001f;
	last = { time, viewDef.renderView.viewID, width, height, true };
	return step;
}

static gbufferLastView_t & RB_LastMainView( const viewDef_t & viewDef ) {
	return backEnd.gbufferLast[( viewDef.renderView.viewEyeBuffer == 1 ) ? 1 : 0];
}

// The HDR main view's place in its eye's history (R14, R18): it follows the frame before when that drew the same
// view at the same size and no later time; else a new epoch starts, and nothing of before may be read.
static void RB_BeginMainViewHistory( const viewDef_t & viewDef ) {
	const gbufferLastView_t & last = RB_LastMainView( viewDef );
	const idScreenRect & v = viewDef.viewport;
	backEnd.viewFollows = last.valid && last.viewID == viewDef.renderView.viewID && last.frame == viewDef.rayTracing.frameCount - 1
		&& last.width == v.GetWidth() && last.height == v.GetHeight() && viewDef.renderView.time[0] >= last.time;
	backEnd.viewEpoch = backEnd.viewFollows ? last.epoch : ++backEnd.historyEpochs;
	backEnd.viewSequence = backEnd.viewFollows ? last.sequence + 1 : 0;
}

// A screenshot draws between two frames of the history: it reads it as the next frame will, and leaves the camera
// and the sequence of the frame before for that one.
static void RB_EndMainViewHistory( const viewDef_t & viewDef ) {
	gbufferLastView_t & last = RB_LastMainView( viewDef );
	if ( tr.takingScreenshot && backEnd.viewFollows ) {
		last.frame = viewDef.rayTracing.frameCount;
		return;
	}
	last.projection = viewDef.projectionRenderMatrix;
	memcpy( last.worldModelView, viewDef.worldSpace.modelViewMatrix, sizeof( last.worldModelView ) );
	last.mvp = viewDef.worldSpace.mvp;
	last.viewID = viewDef.renderView.viewID;
	last.frame = viewDef.rayTracing.frameCount;
	last.width = viewDef.viewport.GetWidth();
	last.height = viewDef.viewport.GetHeight();
	last.time = viewDef.renderView.time[0];
	last.epoch = backEnd.viewEpoch;
	last.sequence = backEnd.viewSequence;
	last.valid = true;
}

// The eye is where clip x, y and w are 0: column 2 of the inverse, whatever the projection (an eye of a stereo pair too).
// The history's part is the HDR main view's, between RB_BeginMainViewHistory and RB_EndMainViewHistory.
rtView_t RB_RayTracingView( const viewDef_t & viewDef ) {
	rtView_t view;
	idRenderMatrix clipToWorld;
	idRenderMatrix::Inverse( viewDef.worldSpace.mvp, clipToWorld );
	const idRenderMatrix & prevMvp = backEnd.viewFollows ? RB_LastMainView( viewDef ).mvp : viewDef.worldSpace.mvp;
	for ( int row = 0; row < 4; row++ ) {
		for ( int column = 0; column < 4; column++ ) {
			view.clipToWorld[row][column] = clipToWorld[row][column];
			view.worldToClip[row][column] = viewDef.worldSpace.mvp[row][column];
			view.prevWorldToClip[row][column] = prevMvp[row][column];
		}
	}
	memcpy( view.worldToView, viewDef.worldSpace.modelViewMatrix, sizeof( view.worldToView ) );
	memcpy( view.viewToClip, viewDef.projectionMatrix, sizeof( view.viewToClip ) );
	memcpy( view.prevWorldToView, view.worldToView, sizeof( view.prevWorldToView ) );
	memcpy( view.prevViewToClip, view.viewToClip, sizeof( view.prevViewToClip ) );
	if ( backEnd.viewFollows ) {
		const gbufferLastView_t & last = RB_LastMainView( viewDef );
		memcpy( view.prevWorldToView, last.worldModelView, sizeof( view.prevWorldToView ) );
		for ( int row = 0; row < 4; row++ ) {
			for ( int column = 0; column < 4; column++ ) {
				view.prevViewToClip[column * 4 + row] = last.projection[row][column];
			}
		}
		view.frameMs = static_cast<float>( viewDef.renderView.time[0] - last.time );
	}
	view.epoch = backEnd.viewEpoch;
	view.sequence = backEnd.viewSequence;
	view.eye = ( viewDef.renderView.viewEyeBuffer == 1 ) ? 1 : 0;
	view.store = !tr.takingScreenshot;
	view.held = backEnd.tracedHeld;
	const float w = ( clipToWorld[3][2] != 0.0f ) ? clipToWorld[3][2] : 1.0f;
	for ( int i = 0; i < 3; i++ ) {
		view.origin[i] = clipToWorld[i][2] / w;
	}
	view.pixelSpread = 2.0f * idMath::Tan( DEG2RAD( viewDef.renderView.fov_y ) * 0.5f ) / static_cast<float>( Max( 1, static_cast<int>( viewDef.viewport.GetHeight() ) ) );
	return view;
}

// r_rtStats: how the main view's frames of the last second came by their shadows.
static void RB_TracedShadowStats( bool traced, bool withoutVolumes, bool held ) {
	static int frames = 0, tracedFrames = 0, bare = 0, heldFrames = 0, lastPrint = 0;
	frames++;
	tracedFrames += traced ? 1 : 0;
	bare += withoutVolumes ? 1 : 0;
	heldFrames += held ? 1 : 0;
	if ( Sys_Milliseconds() - lastPrint >= 1000 ) {
		lastPrint = Sys_Milliseconds();
		common->Printf( "rt shadows: %d frames, %d traced, %d without volumes, %d held\n", frames, tracedFrames, bare, heldFrames );
		frames = tracedFrames = bare = heldFrames = 0;
	}
}

void RB_DrawView( const void *data, const int stereoEye, bool hdrMainView ) {
	const drawSurfsCommand_t * cmd = (const drawSurfsCommand_t *)data;

	backEnd.viewDef = cmd->viewDef;

	// we will need to do a new copyTexSubImage of the screen
	// when a SS_POST_PROCESS material is used
	backEnd.currentRenderCopied = false;

	if ( !RB_ViewDraws( backEnd.viewDef ) ) {
		return;
	}

	backEnd.pc.c_surfaces += backEnd.viewDef->numDrawSurfs;

	// R15: the GPU scene before anything of the view traces it; the second eye sends the same update again.
	// Only the main view sends: a world gathered for another view leaves a gap, and the next delta is refused.
	bool traceable = false;
	if ( hdrMainView && backEnd.viewDef->gpuScene != nullptr ) {
		renderLog.OpenMainBlock( MRB_RT_SCENE );
		traceable = RB_UpdateGpuScene( *backEnd.viewDef->gpuScene );
		renderLog.CloseMainBlock();
	}

	// render the scene; a frame that cannot trace every caster draws its stencil shadows. Where the front end made
	// none, counting on the trace, the lights draw as traced ones and keep the shadows their history holds (R18)
	const rtShadowSettings_t & shadow = backEnd.viewDef->rayTracing.shadow;
	const bool shadows = hdrMainView && backEnd.hdrPhase && shadow.enabled;
	backEnd.gbufferView = hdrMainView && backEnd.hdrPhase && backEnd.viewDef->rayTracing.gbuffer;
	backEnd.tracedShadows = shadows && ( traceable || shadow.WithoutVolumes() );
	backEnd.tracedHeld = backEnd.tracedShadows && !traceable;
	const bool history = hdrMainView && backEnd.hdrPhase;
	backEnd.viewFollows = false;
	if ( history ) {
		RB_BeginMainViewHistory( *backEnd.viewDef );
	}
	RB_DrawViewInternal( cmd->viewDef, stereoEye, !hdrMainView );

	// motion blur is in HDR: the resolve comes after it
	RB_MotionBlur();

	if ( hdrMainView ) {
		const idScreenRect & v = backEnd.viewDef->viewport;
		renderLog.OpenMainBlock( MRB_HDR );
		renderBackend->ResolveHdrScene( backEnd.viewDef->postProcess, { v.x1, v.y1, v.GetWidth(), v.GetHeight() },
			RB_HdrExposureStep( *backEnd.viewDef, stereoEye ) );
		renderLog.CloseMainBlock();
		backEnd.hdrPhase = false;
		backEnd.hdrGamma = false;
		if ( backEnd.gbufferView ) {
			renderBackend->DrawGBufferDebug( backEnd.viewDef->rayTracing.gbufferDebug, { v.x1, v.y1, v.GetWidth(), v.GetHeight() } );
		}
		const rtDebug_t rtDebug = backEnd.viewDef->rayTracing.rtDebug;
		// drawn before the resolve
		const bool inScene = ( rtDebug == rtDebug_t::REFLECTION || rtDebug == rtDebug_t::SHADOWS || rtDebug == rtDebug_t::INDIRECT );
		if ( rtDebug != rtDebug_t::NONE && !inScene && backEnd.viewDef->gpuScene != nullptr ) {
			renderLog.OpenMainBlock( MRB_RT_DEBUG );
			renderBackend->DrawRayTracingDebug( rtDebug, RB_RayTracingView( *backEnd.viewDef ), { v.x1, v.y1, v.GetWidth(), v.GetHeight() } );
			renderLog.CloseMainBlock();
		}
		RB_RenderDebugTools( (drawSurf_t **)&backEnd.viewDef->drawSurfs[0], backEnd.viewDef->numDrawSurfs );
	}
	if ( history ) {
		RB_EndMainViewHistory( *backEnd.viewDef );
	}
	// what the front end's next views go by: the main view that asked for traced shadows got them, and so did
	// every view that has no volumes
	if ( shadow.enabled && ( hdrMainView || shadow.WithoutVolumes() ) ) {
		const gpuSceneFrame_t * const scene = backEnd.viewDef->gpuScene;
		const bool traced = backEnd.tracedShadows && !backEnd.tracedHeld;
		backEnd.shadowLatch.View( ( scene != nullptr ) ? scene->sceneId : 0, ( scene != nullptr ) ? scene->sequence : 0, traced );
		if ( r_rtStats.GetBool() ) {
			RB_TracedShadowStats( traced, shadow.WithoutVolumes(), backEnd.tracedHeld );
			// the frames around a level load, a refused update or a view the back end takes for another: each one
			if ( !shadow.keepVolumes && ( !traced || !shadow.WithoutVolumes() ) ) {
				common->Printf( "rt shadows: frame %d, scene %u.%u%s: %s, %s (main view %d, hdr scene %d, traceable %d)\n", backEnd.viewDef->rayTracing.frameCount,
					( scene != nullptr ) ? scene->sceneId : 0, ( scene != nullptr ) ? scene->sequence : 0, ( scene != nullptr && scene->full ) ? " full" : "",
					shadow.WithoutVolumes() ? "no volumes" : "volumes", traced ? "traced" : backEnd.tracedHeld ? "held" : "not traced",
					hdrMainView, shadows, traceable );
			}
		}
	}
	backEnd.gbufferView = false;
	backEnd.tracedShadows = false;
	backEnd.tracedHeld = false;
	backEnd.viewFollows = false;

	// optionally draw a box colored based on the eye number
	if ( r_drawEyeColor.GetBool() ) {
		const idScreenRect & r = backEnd.viewDef->viewport;
		renderBackend->Scissor( ( r.x1 + r.x2 ) / 2, ( r.y1 + r.y2 ) / 2, 32, 32 );
		switch ( stereoEye ) {
			case -1:
				renderBackend->Clear( true, false, false, 0, 1.0f, 0.0f, 0.0f, 1.0f );
				break;
			case 1:
				renderBackend->Clear( true, false, false, 0, 0.0f, 1.0f, 0.0f, 1.0f );
				break;
			default:
				renderBackend->Clear( true, false, false, 0, 0.5f, 0.5f, 0.5f, 1.0f );
				break;
		}
	}
}

/*
==================
RB_CopyRender

Copy part of the current framebuffer to an image
==================
*/
void RB_CopyRender( const void *data ) {
	const copyRenderCommand_t * cmd = (const copyRenderCommand_t *)data;

	if ( r_skipCopyTexture.GetBool() ) {
		return;
	}

	RENDERLOG_PRINTF( "***************** RB_CopyRender *****************\n" );

	renderLog.OpenMainBlock( MRB_CAPTURE_COLORBUFFER );
	renderLog.OpenBlock( "RB_CopyRender" );

	if ( cmd->image ) {
		cmd->image->CopyFramebuffer( cmd->x, cmd->y, cmd->imageWidth, cmd->imageHeight );
	}

	if ( cmd->clearColorAfterCopy ) {
		renderBackend->Clear( true, false, false, STENCIL_SHADOW_TEST_VALUE, 0, 0, 0, 0 );
	}

	renderLog.CloseBlock();
	renderLog.CloseMainBlock();
}

/*
==================
RB_PostProcess

==================
*/
extern idCVar rs_enable;
void RB_PostProcess( const void * data ) {

	// only do the post process step if resolution scaling is enabled. Prevents the unnecessary copying of the framebuffer and
	// corresponding full screen quad pass.
	if ( rs_enable.GetInteger() == 0 ) {
		return;
	}

	renderLog.OpenMainBlock( MRB_POSTPROCESS );
	renderLog.OpenBlock( "RB_PostProcess" );

	// resolve the scaled rendering to a temporary texture
	postProcessCommand_t * cmd = (postProcessCommand_t *)data;
	const idScreenRect & viewport = cmd->viewDef->viewport;
	globalImages->currentRenderImage->CopyFramebuffer( viewport.x1, viewport.y1, viewport.GetWidth(), viewport.GetHeight() );

	renderBackend->State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO | GLS_DEPTHMASK | GLS_DEPTHFUNC_ALWAYS );
	renderBackend->Cull( CT_TWO_SIDED );

	int screenWidth = renderSystem->GetWidth();
	int screenHeight = renderSystem->GetHeight();

	// set the window clipping
	renderBackend->Viewport( 0, 0, screenWidth, screenHeight );
	renderBackend->Scissor( 0, 0, screenWidth, screenHeight );

	globalImages->currentRenderImage->Bind( 0 );
	renderProgManager.BindShader_PostProcess();

	// Draw
	RB_DrawElementsWithCounters( &backEnd.unitSquareSurface );

	renderLog.CloseBlock();
	renderLog.CloseMainBlock();
}
