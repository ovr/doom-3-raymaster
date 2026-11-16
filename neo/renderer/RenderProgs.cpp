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



idRenderProgManager renderProgManager;

/*
================================================================================================
R_ProgBackendCallable

The render backend belongs to the main thread. Programs are loaded there today; a caller on
another thread never reached OpenGL either, its calls had no context.
================================================================================================
*/
static bool R_ProgBackendCallable( [[maybe_unused]] const char * progName ) {
	if ( renderBackend == nullptr ) {
		return false;
	}
	if ( renderBackend->IsOwnerThread() ) {
		return true;
	}
#ifdef _DEBUG
	// reported at 1, 2, 4, 8, ... so that a log shows whether it happens and how often
	static idSysInterlockedInteger skipped;
	const int count = skipped.Increment();
	if ( ( count & ( count - 1 ) ) == 0 ) {
		common->Warning( "idRenderProgManager: %d render backend calls skipped off the main thread, the last for '%s'", count, progName );
	}
#endif
	return false;
}

/*
================================================================================================
idRenderProgManager::idRenderProgManager()
================================================================================================
*/
idRenderProgManager::idRenderProgManager() {
}

/*
================================================================================================
idRenderProgManager::~idRenderProgManager()
================================================================================================
*/
idRenderProgManager::~idRenderProgManager() {
}

/*
================================================================================================
R_ReloadShaders
================================================================================================
*/
static void R_ReloadShaders( const idCmdArgs &args ) {	
	renderProgManager.KillAllShaders();
	renderProgManager.LoadAllShaders();
}

/*
================================================================================================
idRenderProgManager::Init()
================================================================================================
*/
void idRenderProgManager::Init() {
	common->Printf( "----- Initializing Render Shaders -----\n" );


	for ( int i = 0; i < MAX_BUILTINS; i++ ) {
		builtinShaders[i] = -1;
	}
	struct builtinShaders_t {
		int index;
		const char * name;
	} builtins[] = {
		{ BUILTIN_GUI, "gui.vfp" },
		{ BUILTIN_COLOR, "color.vfp" },
		{ BUILTIN_SIMPLESHADE, "simpleshade.vfp" },
		{ BUILTIN_TEXTURED, "texture.vfp" },
		{ BUILTIN_TEXTURE_VERTEXCOLOR, "texture_color.vfp" },
		{ BUILTIN_TEXTURE_VERTEXCOLOR_SKINNED, "texture_color_skinned.vfp" },
		{ BUILTIN_TEXTURE_TEXGEN_VERTEXCOLOR, "texture_color_texgen.vfp" },
		{ BUILTIN_INTERACTION, "interaction.vfp" },
		{ BUILTIN_INTERACTION_SKINNED, "interaction_skinned.vfp" },
		{ BUILTIN_INTERACTION_AMBIENT, "interactionAmbient.vfp" },
		{ BUILTIN_INTERACTION_AMBIENT_SKINNED, "interactionAmbient_skinned.vfp" },
		{ BUILTIN_ENVIRONMENT, "environment.vfp" },
		{ BUILTIN_ENVIRONMENT_SKINNED, "environment_skinned.vfp" },
		{ BUILTIN_BUMPY_ENVIRONMENT, "bumpyEnvironment.vfp" },
		{ BUILTIN_BUMPY_ENVIRONMENT_SKINNED, "bumpyEnvironment_skinned.vfp" },

		{ BUILTIN_DEPTH, "depth.vfp" },
		{ BUILTIN_DEPTH_SKINNED, "depth_skinned.vfp" },
		{ BUILTIN_SHADOW_DEBUG, "shadowDebug.vfp" },
		{ BUILTIN_SHADOW_DEBUG_SKINNED, "shadowDebug_skinned.vfp" },

		{ BUILTIN_BLENDLIGHT, "blendlight.vfp" },
		{ BUILTIN_FOG, "fog.vfp" },
		{ BUILTIN_FOG_SKINNED, "fog_skinned.vfp" },
		{ BUILTIN_SKYBOX, "skybox.vfp" },
		{ BUILTIN_WOBBLESKY, "wobblesky.vfp" },
		{ BUILTIN_POSTPROCESS, "postprocess.vfp" },
		{ BUILTIN_STEREO_DEGHOST, "stereoDeGhost.vfp" },
		{ BUILTIN_STEREO_WARP, "stereoWarp.vfp" },
		{ BUILTIN_ZCULL_RECONSTRUCT, "zcullReconstruct.vfp" },
		{ BUILTIN_BINK, "bink.vfp" },
		{ BUILTIN_BINK_GUI, "bink_gui.vfp" },
		{ BUILTIN_STEREO_INTERLACE, "stereoInterlace.vfp" },
		{ BUILTIN_MOTION_BLUR, "motionBlur.vfp" },
	};
	int numBuiltins = sizeof( builtins ) / sizeof( builtins[0] );
	vertexShaders.SetNum( numBuiltins );
	fragmentShaders.SetNum( numBuiltins );
	glslPrograms.SetNum( numBuiltins );

	for ( int i = 0; i < numBuiltins; i++ ) {
		vertexShaders[i].name = builtins[i].name;
		fragmentShaders[i].name = builtins[i].name;
		builtinShaders[builtins[i].index] = i;
		LoadVertexShader( i );
		LoadFragmentShader( i );
		LoadGLSLProgram( i, i, i );
	}

	// Special case handling for fastZ shaders
	builtinShaders[BUILTIN_SHADOW] = FindVertexShader( "shadow.vp" );
	builtinShaders[BUILTIN_SHADOW_SKINNED] = FindVertexShader( "shadow_skinned.vp" );

	FindGLSLProgram( "shadow.vp", builtinShaders[BUILTIN_SHADOW], -1 );
	FindGLSLProgram( "shadow_skinned.vp", builtinShaders[BUILTIN_SHADOW_SKINNED], -1 );

	glslUniforms.SetNum( RENDERPARM_USER + MAX_GLSL_USER_PARMS, vec4_zero );

	vertexShaders[builtinShaders[BUILTIN_TEXTURE_VERTEXCOLOR_SKINNED]].usesJoints = true;
	vertexShaders[builtinShaders[BUILTIN_INTERACTION_SKINNED]].usesJoints = true;
	vertexShaders[builtinShaders[BUILTIN_INTERACTION_AMBIENT_SKINNED]].usesJoints = true;
	vertexShaders[builtinShaders[BUILTIN_ENVIRONMENT_SKINNED]].usesJoints = true;
	vertexShaders[builtinShaders[BUILTIN_BUMPY_ENVIRONMENT_SKINNED]].usesJoints = true;
	vertexShaders[builtinShaders[BUILTIN_DEPTH_SKINNED]].usesJoints = true;
	vertexShaders[builtinShaders[BUILTIN_SHADOW_SKINNED]].usesJoints = true;
	vertexShaders[builtinShaders[BUILTIN_SHADOW_DEBUG_SKINNED]].usesJoints = true;
	vertexShaders[builtinShaders[BUILTIN_FOG_SKINNED]].usesJoints = true;

	cmdSystem->AddCommand( "reloadShaders", R_ReloadShaders, CMD_FL_RENDERER, "reloads shaders" );
}

/*
================================================================================================
idRenderProgManager::LoadAllShaders()
================================================================================================
*/
void idRenderProgManager::LoadAllShaders() {
	for ( int i = 0; i < vertexShaders.Num(); i++ ) {
		LoadVertexShader( i );
	}
	for ( int i = 0; i < fragmentShaders.Num(); i++ ) {
		LoadFragmentShader( i );
	}

	for ( int i = 0; i < glslPrograms.Num(); ++i ) {
		LoadGLSLProgram( i, glslPrograms[i].vertexShaderIndex, glslPrograms[i].fragmentShaderIndex );
	}
}

/*
================================================================================================
idRenderProgManager::KillAllShaders()
================================================================================================
*/
void idRenderProgManager::KillAllShaders() {
	Unbind();
	// off the main thread the stages and programs leak, as their OpenGL names did
	const bool callable = R_ProgBackendCallable( "KillAllShaders" );
	for ( int i = 0; i < vertexShaders.Num(); i++ ) {
		if ( vertexShaders[i].handle != shaderHandle_t::NONE ) {
			if ( callable ) {
				renderBackend->DestroyShaderStage( vertexShaders[i].handle );
			}
			vertexShaders[i].handle = shaderHandle_t::NONE;
		}
	}
	for ( int i = 0; i < fragmentShaders.Num(); i++ ) {
		if ( fragmentShaders[i].handle != shaderHandle_t::NONE ) {
			if ( callable ) {
				renderBackend->DestroyShaderStage( fragmentShaders[i].handle );
			}
			fragmentShaders[i].handle = shaderHandle_t::NONE;
		}
	}
	for ( int i = 0; i < glslPrograms.Num(); ++i ) {
		if ( glslPrograms[i].handle != programHandle_t::NONE ) {
			if ( callable ) {
				renderBackend->DestroyProgram( glslPrograms[i].handle );
			}
			glslPrograms[i].handle = programHandle_t::NONE;
		}
	}
}

/*
================================================================================================
idRenderProgManager::Shutdown()
================================================================================================
*/
void idRenderProgManager::Shutdown() {
	KillAllShaders();
}

/*
================================================================================================
idRenderProgManager::FindVertexShader
================================================================================================
*/
int idRenderProgManager::FindVertexShader( const char * name ) {
	for ( int i = 0; i < vertexShaders.Num(); i++ ) {
		if ( vertexShaders[i].name.Icmp( name ) == 0 ) {
			LoadVertexShader( i );
			return i;
		}
	}
	vertexShader_t shader;
	shader.name = name;
	int index = vertexShaders.Append( shader );
	LoadVertexShader( index );
	currentVertexShader = index;

	// FIXME: we should really scan the program source code for using rpEnableSkinning but at this
	// point we directly load a binary and the program source code is not available on the consoles
	if (	idStr::Icmp( name, "heatHaze.vfp" ) == 0 ||
			idStr::Icmp( name, "heatHazeWithMask.vfp" ) == 0 ||
			idStr::Icmp( name, "heatHazeWithMaskAndVertex.vfp" ) == 0 ) {
		vertexShaders[index].usesJoints = true;
		vertexShaders[index].optionalSkinning = true;
	}

	return index;
}

/*
================================================================================================
idRenderProgManager::FindFragmentShader
================================================================================================
*/
int idRenderProgManager::FindFragmentShader( const char * name ) {
	for ( int i = 0; i < fragmentShaders.Num(); i++ ) {
		if ( fragmentShaders[i].name.Icmp( name ) == 0 ) {
			LoadFragmentShader( i );
			return i;
		}
	}
	fragmentShader_t shader;
	shader.name = name;
	int index = fragmentShaders.Append( shader );
	LoadFragmentShader( index );
	currentFragmentShader = index;
	return index;
}




/*
================================================================================================
idRenderProgManager::LoadVertexShader
================================================================================================
*/
void idRenderProgManager::LoadVertexShader( int index ) {
	if ( vertexShaders[index].handle != shaderHandle_t::NONE ) {
		return; // Already loaded
	}
	if ( !R_ProgBackendCallable( vertexShaders[index].name ) ) {
		return;
	}
	vertexShaders[index].handle = renderBackend->CreateShaderStage( shaderStageType_t::VERTEX, vertexShaders[index].name, vertexShaders[index].info );
}

/*
================================================================================================
idRenderProgManager::LoadFragmentShader
================================================================================================
*/
void idRenderProgManager::LoadFragmentShader( int index ) {
	if ( fragmentShaders[index].handle != shaderHandle_t::NONE ) {
		return; // Already loaded
	}
	if ( !R_ProgBackendCallable( fragmentShaders[index].name ) ) {
		return;
	}
	fragmentShaders[index].handle = renderBackend->CreateShaderStage( shaderStageType_t::FRAGMENT, fragmentShaders[index].name, fragmentShaders[index].info );
}

/*
================================================================================================
idRenderProgManager::FindGLSLProgram
================================================================================================
*/
int	 idRenderProgManager::FindGLSLProgram( const char * name, int vIndex, int fIndex ) {

	for ( int i = 0; i < glslPrograms.Num(); ++i ) {
		if ( ( glslPrograms[i].vertexShaderIndex == vIndex ) && ( glslPrograms[i].fragmentShaderIndex == fIndex ) ) {
			LoadGLSLProgram( i, vIndex, fIndex );
			return i;
		}
	}

	renderProg_t program;
	program.name = name;
	int index = glslPrograms.Append( program );
	LoadGLSLProgram( index, vIndex, fIndex );
	return index;
}

/*
================================================================================================
idRenderProgManager::LoadGLSLProgram

The name and the stage indexes are written only after the backend linked the program. A failed
link in OpenGL is an error that leaves the entry without them, so no later search finds it.
================================================================================================
*/
void idRenderProgManager::LoadGLSLProgram( const int programIndex, const int vertexShaderIndex, const int fragmentShaderIndex ) {
	renderProg_t & prog = glslPrograms[programIndex];

	if ( prog.handle != programHandle_t::NONE ) {
		return; // Already loaded
	}

	idStr programName = ( vertexShaderIndex >= 0 ) ? vertexShaders[ vertexShaderIndex ].name.c_str() : "<Invalid>";
	programName.StripFileExtension();
	if ( !R_ProgBackendCallable( programName ) ) {
		return;
	}

	const shaderHandle_t vertex = ( vertexShaderIndex != -1 ) ? vertexShaders[ vertexShaderIndex ].handle : shaderHandle_t::NONE;
	const shaderHandle_t fragment = ( fragmentShaderIndex != -1 ) ? fragmentShaders[ fragmentShaderIndex ].handle : shaderHandle_t::NONE;

	prog.handle = renderBackend->CreateProgram( vertex, fragment, programName, prog.info );
	prog.name = programName;
	prog.fragmentShaderIndex = fragmentShaderIndex;
	prog.vertexShaderIndex = vertexShaderIndex;
}

/*
================================================================================================
idRenderProgManager::BindShader
================================================================================================
*/
void idRenderProgManager::BindShader( int vIndex, int fIndex ) {
	if ( currentVertexShader == vIndex && currentFragmentShader == fIndex ) {
		return;
	}
	currentVertexShader = vIndex;
	currentFragmentShader = fIndex;
	// vIndex denotes the GLSL program
	if ( vIndex >= 0 && vIndex < glslPrograms.Num() ) {
		currentRenderProgram = vIndex;
		RENDERLOG_PRINTF( "Binding GLSL Program %s\n", glslPrograms[vIndex].name.c_str() );
		// a program without a handle leaves the previous one in use, as binding a bad OpenGL name did
		if ( glslPrograms[vIndex].handle != programHandle_t::NONE ) {
			renderBackend->BindProgram( glslPrograms[vIndex].handle );
		}
	}
}

/*
================================================================================================
idRenderProgManager::Unbind
================================================================================================
*/
void idRenderProgManager::Unbind() {
	currentVertexShader = -1;
	currentFragmentShader = -1;

	if ( R_ProgBackendCallable( "Unbind" ) ) {
		renderBackend->BindProgram( programHandle_t::NONE );
	}
}

/*
================================================================================================
idRenderProgManager::SetRenderParms
================================================================================================
*/
void idRenderProgManager::SetRenderParms( renderParm_t rp, const float * value, int num ) {
	for ( int i = 0; i < num; i++ ) {
		SetRenderParm( (renderParm_t)(rp + i), value + ( i * 4 ) );
	}
}

/*
================================================================================================
idRenderProgManager::SetRenderParm
================================================================================================
*/
void idRenderProgManager::SetRenderParm( renderParm_t rp, const float * value ) {
	SetUniformValue( rp, value );
}

/*
================================================================================================
idRenderProgManager::SetUniformValue
================================================================================================
*/
void idRenderProgManager::SetUniformValue( const renderParm_t rp, const float * value ) {
	for ( int i = 0; i < 4; i++ ) {
		glslUniforms[rp][i] = value[i];
	}
}

/*
================================================================================================
idRenderProgManager::ZeroUniforms
================================================================================================
*/
void idRenderProgManager::ZeroUniforms() {
	memset( glslUniforms.Ptr(), 0, glslUniforms.Allocated() );
}

