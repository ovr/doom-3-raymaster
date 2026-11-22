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
#ifndef __RENDERPROGS_H__
#define __RENDERPROGS_H__

#include <array>

#include "RenderParms.h"
#include "RenderProgTable.h"


/*
================================================================================================
idRenderProgManager
================================================================================================
*/
class idRenderProgManager {
public:
	idRenderProgManager();
	virtual ~idRenderProgManager();

	void	Init();
	void	Shutdown();

	void	SetRenderParm( renderParm_t rp, const float * value );
	void	SetRenderParms( renderParm_t rp, const float * values, int numValues );

	int		FindVertexShader( const char * name );
	int		FindFragmentShader( const char * name );

	void	BindShader( int vIndex, int fIndex );

	void	BindShader_GUI( ) { BindShader_Builtin( renderProgBuiltin_t::GUI ); }
	void	BindShader_Color( ) { BindShader_Builtin( renderProgBuiltin_t::COLOR ); }
	void	BindShader_Texture( ) { BindShader_Builtin( renderProgBuiltin_t::TEXTURED ); }
	void	BindShader_TextureVertexColor() { BindShader_Builtin( renderProgBuiltin_t::TEXTURE_VERTEXCOLOR ); };
	void	BindShader_TextureVertexColorSkinned() { BindShader_Builtin( renderProgBuiltin_t::TEXTURE_VERTEXCOLOR_SKINNED ); };
	void	BindShader_TextureVertexColorSoft() { BindShader_Builtin( renderProgBuiltin_t::TEXTURE_VERTEXCOLOR_SOFT ); }
	void	BindShader_TextureTexGenVertexColor() { BindShader_Builtin( renderProgBuiltin_t::TEXTURE_TEXGEN_VERTEXCOLOR ); };
	void	BindShader_Interaction()  { BindShader_Builtin( renderProgBuiltin_t::INTERACTION ); }
	void	BindShader_InteractionSkinned()  { BindShader_Builtin( renderProgBuiltin_t::INTERACTION_SKINNED ); }
	void	BindShader_InteractionAmbient()  { BindShader_Builtin( renderProgBuiltin_t::INTERACTION_AMBIENT ); }
	void	BindShader_InteractionAmbientSkinned()  { BindShader_Builtin( renderProgBuiltin_t::INTERACTION_AMBIENT_SKINNED ); }
	void	BindShader_SimpleShade() { BindShader_Builtin( renderProgBuiltin_t::SIMPLESHADE ); }
	void	BindShader_Environment() { BindShader_Builtin( renderProgBuiltin_t::ENVIRONMENT ); }
	void	BindShader_EnvironmentSkinned() { BindShader_Builtin( renderProgBuiltin_t::ENVIRONMENT_SKINNED ); }
	void	BindShader_BumpyEnvironment() { BindShader_Builtin( renderProgBuiltin_t::BUMPY_ENVIRONMENT ); }
	void	BindShader_BumpyEnvironmentSkinned() { BindShader_Builtin( renderProgBuiltin_t::BUMPY_ENVIRONMENT_SKINNED ); }

	void	BindShader_Depth() { BindShader_Builtin( renderProgBuiltin_t::DEPTH ); }
	void	BindShader_DepthSkinned() { BindShader_Builtin( renderProgBuiltin_t::DEPTH_SKINNED ); }
	void	BindShader_Shadow() { BindShader( Builtin( renderProgBuiltin_t::SHADOW ), -1 ); }
	void	BindShader_ShadowSkinned() { BindShader( Builtin( renderProgBuiltin_t::SHADOW_SKINNED ), -1 ); }
	void	BindShader_ShadowDebug() { BindShader_Builtin( renderProgBuiltin_t::SHADOW_DEBUG ); }
	void	BindShader_ShadowDebugSkinned() { BindShader_Builtin( renderProgBuiltin_t::SHADOW_DEBUG_SKINNED ); }

	void	BindShader_BlendLight() { BindShader_Builtin( renderProgBuiltin_t::BLENDLIGHT ); }
	void	BindShader_Fog() { BindShader_Builtin( renderProgBuiltin_t::FOG ); }
	void	BindShader_FogSkinned() { BindShader_Builtin( renderProgBuiltin_t::FOG_SKINNED ); }
	void	BindShader_SkyBox() { BindShader_Builtin( renderProgBuiltin_t::SKYBOX ); }
	void	BindShader_WobbleSky() { BindShader_Builtin( renderProgBuiltin_t::WOBBLESKY ); }
	void	BindShader_StereoDeGhost() { BindShader_Builtin( renderProgBuiltin_t::STEREO_DEGHOST ); }
	void	BindShader_StereoWarp() { BindShader_Builtin( renderProgBuiltin_t::STEREO_WARP ); }
	void	BindShader_StereoInterlace() { BindShader_Builtin( renderProgBuiltin_t::STEREO_INTERLACE ); }
	void	BindShader_PostProcess() { BindShader_Builtin( renderProgBuiltin_t::POSTPROCESS ); }
	void	BindShader_ZCullReconstruct() { BindShader_Builtin( renderProgBuiltin_t::ZCULL_RECONSTRUCT ); }
	void	BindShader_Bink() { BindShader_Builtin( renderProgBuiltin_t::BINK ); }
	void	BindShader_BinkGUI() { BindShader_Builtin( renderProgBuiltin_t::BINK_GUI ); }
	void	BindShader_MotionBlur() { BindShader_Builtin( renderProgBuiltin_t::MOTION_BLUR ); }

	// the joints buffer should only be bound for vertex programs that use joints
	bool	ShaderUsesJoints() const { return vertexShaders[currentVertexShader].usesJoints; }
	// the rpEnableSkinning render parm should only be set for vertex programs that use it
	bool	ShaderHasOptionalSkinning() const { return vertexShaders[currentVertexShader].optionalSkinning; }

	// unbind the currently bound render program
	void	Unbind();

	// this should only be called via the reload shader console command
	void	LoadAllShaders();
	void	KillAllShaders();

	static const int	MAX_GLSL_USER_PARMS = NUM_USER_RENDERPARMS;
	int			GetGLSLCurrentProgram() const { return currentRenderProgram; }
	void		SetUniformValue( const renderParm_t rp, const float * value );
	// the parm table a draw hands to the backend: MAX_RENDER_PARMS vectors
	const idVec4 *	GetRenderParms() const { return renderParms.Ptr(); }
	int			FindGLSLProgram( const char* name, int vIndex, int fIndex );
	void		ZeroUniforms();

	// One line per program in load order: its stages and what the backend reported for them
	[[nodiscard]] idStrList	ProgramInfoLines() const;

protected:
	void	LoadVertexShader( int index );
	void	LoadFragmentShader( int index );

	// the stage and program index of each builtin, -1 until Init made it
	std::array<int, static_cast<size_t>( renderProgBuiltin_t::COUNT )> builtinShaders;
	[[nodiscard]] int Builtin( renderProgBuiltin_t builtin ) const { return builtinShaders[static_cast<size_t>( builtin )]; }
	void BindShader_Builtin( renderProgBuiltin_t builtin ) { BindShader( Builtin( builtin ), Builtin( builtin ) ); }

	void	LoadGLSLProgram( const int programIndex, const int vertexShaderIndex, const int fragmentShaderIndex );

	// a stage or a program of the render backend, with what the backend said it reads
	struct vertexShader_t {
		idStr				name;
		shaderHandle_t		handle = shaderHandle_t::NONE;
		shaderStageInfo_t	info = {};
		bool				usesJoints = false;			// the program table's skinned variants and optional skinning
		bool				optionalSkinning = false;
	};
	[[nodiscard]] static vertexShader_t	NewVertexShader( const char * name );
	void	LoadBuiltin( const renderProgDef_t & program, bool skinned );
	struct fragmentShader_t {
		idStr				name;
		shaderHandle_t		handle = shaderHandle_t::NONE;
		shaderStageInfo_t	info = {};
	};

	struct renderProg_t {
		idStr				name;
		programHandle_t		handle = programHandle_t::NONE;
		int					vertexShaderIndex = -1;
		int					fragmentShaderIndex = -1;
		programInfo_t		info = {};
	};
	int	currentRenderProgram;
	idList<renderProg_t, TAG_RENDER> renderProgs;
	idStaticList<idVec4, RENDERPARM_USER + MAX_GLSL_USER_PARMS> renderParms;


	int				currentVertexShader;
	int				currentFragmentShader;
	idList<vertexShader_t, TAG_RENDER> vertexShaders;
	idList<fragmentShader_t, TAG_RENDER> fragmentShaders;
};

extern idRenderProgManager renderProgManager;

static_assert( MAX_RENDER_PARMS == RENDERPARM_USER + idRenderProgManager::MAX_GLSL_USER_PARMS );

#endif
