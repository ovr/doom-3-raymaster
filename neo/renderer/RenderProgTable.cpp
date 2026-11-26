/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company. 
Copyright (C) 2026 Dmitry Patsura

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

#include "RenderProgTable.h"

namespace {

using enum renderProgBuiltin_t;

// The builtins in the order Init creates them (then the vertex-only programs, then our own).
constexpr renderProgDef_t renderProgs[] = {
	{ .name = "gui", .builtin = GUI },
	{ .name = "color", .builtin = COLOR },
	{ .name = "simpleshade", .builtin = SIMPLESHADE },
	{ .name = "texture", .builtin = TEXTURED },
	{ .name = "texture_color", .builtin = TEXTURE_VERTEXCOLOR, .skinned = TEXTURE_VERTEXCOLOR_SKINNED },
	{ .name = "texture_color_texgen", .builtin = TEXTURE_TEXGEN_VERTEXCOLOR },
	{ .name = "interaction", .builtin = INTERACTION, .skinned = INTERACTION_SKINNED },
	{ .name = "interactionAmbient", .builtin = INTERACTION_AMBIENT, .skinned = INTERACTION_AMBIENT_SKINNED },
	{ .name = "environment", .builtin = ENVIRONMENT, .skinned = ENVIRONMENT_SKINNED },
	{ .name = "bumpyEnvironment", .builtin = BUMPY_ENVIRONMENT, .skinned = BUMPY_ENVIRONMENT_SKINNED },
	{ .name = "depth", .builtin = DEPTH, .skinned = DEPTH_SKINNED },
	{ .name = "shadow", .builtin = SHADOW, .skinned = SHADOW_SKINNED, .fragment = false },
	{ .name = "shadowDebug", .builtin = SHADOW_DEBUG, .skinned = SHADOW_DEBUG_SKINNED },
	{ .name = "blendlight", .builtin = BLENDLIGHT },
	{ .name = "fog", .builtin = FOG, .skinned = FOG_SKINNED },
	{ .name = "skybox", .builtin = SKYBOX },
	{ .name = "wobblesky", .builtin = WOBBLESKY },
	{ .name = "postprocess", .builtin = POSTPROCESS },
	{ .name = "stereoDeGhost", .builtin = STEREO_DEGHOST },
	{ .name = "stereoWarp", .builtin = STEREO_WARP },
	{ .name = "zcullReconstruct", .builtin = ZCULL_RECONSTRUCT },
	{ .name = "bink", .builtin = BINK },
	{ .name = "bink_gui", .builtin = BINK_GUI },
	{ .name = "stereoInterlace", .builtin = STEREO_INTERLACE },
	{ .name = "motionBlur", .builtin = MOTION_BLUR },
	{ .name = "gbuffer", .builtin = GBUFFER, .skinned = GBUFFER_SKINNED, .own = true },
	{ .name = "interaction_rt", .builtin = INTERACTION_RT, .skinned = INTERACTION_RT_SKINNED, .own = true, .traces = true },

	// the programs retail materials name
	{ .name = "heatHaze", .optionalSkinning = true },
	{ .name = "heatHazeWithMask", .optionalSkinning = true },
	{ .name = "heatHazeWithMaskAndVertex", .optionalSkinning = true },
	{ .name = "bloodorb1_capture" },
	{ .name = "bloodorb2_capture" },
	{ .name = "bloodorb3_capture" },
	{ .name = "bloodorb_draw" },
	{ .name = "enviroSuit" },
	{ .name = "colorProcess" },
};

// the uniform names of the render parms, in renderParm_t order
constexpr const char * renderParmNames[] = {
	"rpScreenCorrectionFactor",
	"rpWindowCoord",
	"rpDiffuseModifier",
	"rpSpecularModifier",

	"rpLocalLightOrigin",
	"rpLocalViewOrigin",

	"rpLightProjectionS",
	"rpLightProjectionT",
	"rpLightProjectionQ",
	"rpLightFalloffS",

	"rpBumpMatrixS",
	"rpBumpMatrixT",

	"rpDiffuseMatrixS",
	"rpDiffuseMatrixT",

	"rpSpecularMatrixS",
	"rpSpecularMatrixT",

	"rpVertexColorModulate",
	"rpVertexColorAdd",

	"rpColor",
	"rpViewOrigin",
	"rpGlobalEyePos",

	"rpMVPmatrixX",
	"rpMVPmatrixY",
	"rpMVPmatrixZ",
	"rpMVPmatrixW",

	"rpModelMatrixX",
	"rpModelMatrixY",
	"rpModelMatrixZ",
	"rpModelMatrixW",

	"rpProjectionMatrixX",
	"rpProjectionMatrixY",
	"rpProjectionMatrixZ",
	"rpProjectionMatrixW",

	"rpModelViewMatrixX",
	"rpModelViewMatrixY",
	"rpModelViewMatrixZ",
	"rpModelViewMatrixW",

	"rpTextureMatrixS",
	"rpTextureMatrixT",

	"rpTexGen0S",
	"rpTexGen0T",
	"rpTexGen0Q",
	"rpTexGen0Enabled",

	"rpTexGen1S",
	"rpTexGen1T",
	"rpTexGen1Q",
	"rpTexGen1Enabled",

	"rpWobbleSkyX",
	"rpWobbleSkyY",
	"rpWobbleSkyZ",

	"rpOverbright",
	"rpEnableSkinning",
	"rpAlphaTest",

	"rpPrevMVPmatrixX",
	"rpPrevMVPmatrixY",
	"rpPrevMVPmatrixZ",
	"rpPrevMVPmatrixW",
	"rpJointMotion",
	"rpGBufferParms",

	"rpRtLight",
	"rpRtShadow"
};
static_assert( std::size( renderParmNames ) == RENDERPARM_TOTAL );

constexpr const char * userParmNames[] = {
	"rpUser0", "rpUser1", "rpUser2", "rpUser3", "rpUser4", "rpUser5", "rpUser6", "rpUser7"
};
static_assert( std::size( userParmNames ) == NUM_USER_RENDERPARMS );

// in the order the GL path bound them
constexpr renderProgVertexInput_t vertexInputs[] = {
	{ "POSITION",	PC_ATTRIB_INDEX_VERTEX },
	{ "TEXCOORD0",	PC_ATTRIB_INDEX_ST },
	{ "NORMAL",		PC_ATTRIB_INDEX_NORMAL },
	{ "TANGENT",	PC_ATTRIB_INDEX_TANGENT },
	{ "COLOR0",		PC_ATTRIB_INDEX_COLOR },
	{ "COLOR1",		PC_ATTRIB_INDEX_COLOR2 },
};

}	// namespace

std::span<const renderProgDef_t> R_RenderProgs() {
	return renderProgs;
}

std::optional<renderProgStage_t> R_FindRenderProg( std::string_view stageName ) {
	idStr name( stageName.data(), 0, static_cast<int>( stageName.size() ) );
	name.StripFileExtension();
	constexpr std::string_view SKINNED = "_skinned";
	for ( const renderProgDef_t & program : renderProgs ) {
		if ( name.Icmp( program.name ) == 0 ) {
			return renderProgStage_t { &program, false };
		}
		const int length = static_cast<int>( strlen( program.name ) );
		if ( program.skinned != renderProgBuiltin_t::NONE && name.Length() == length + static_cast<int>( SKINNED.size() )
				&& idStr::Icmpn( name, program.name, length ) == 0 && idStr::Icmp( name.c_str() + length, SKINNED.data() ) == 0 ) {
			return renderProgStage_t { &program, true };
		}
	}
	return std::nullopt;
}

idStr R_RenderProgStageName( const renderProgDef_t & program, bool skinned ) {
	return va( "%s%s%s", program.name, skinned ? "_skinned" : "", program.fragment ? ".vfp" : ".vp" );
}

bool R_RenderProgHasStage( std::string_view stageName, shaderStageType_t stage ) {
	const std::optional<renderProgStage_t> found = R_FindRenderProg( stageName );
	return found && ( stage == shaderStageType_t::VERTEX || found->program->fragment );
}

std::span<const renderProgVertexInput_t> R_VertexInputs() {
	return vertexInputs;
}

int R_VertexAttribIndex( std::string_view semantic ) {
	for ( const renderProgVertexInput_t & input : vertexInputs ) {
		if ( semantic == input.semantic ) {
			return input.index;
		}
	}
	return -1;
}

const char * R_RenderParmName( renderParm_t parm ) {
	if ( parm >= RENDERPARM_USER ) {
		assert( parm < RENDERPARM_USER + NUM_USER_RENDERPARMS );
		return userParmNames[parm - RENDERPARM_USER];
	}
	assert( parm < RENDERPARM_TOTAL );
	return renderParmNames[parm];
}

std::optional<renderParm_t> R_FindRenderParm( std::string_view name ) {
	for ( int i = 0; i < RENDERPARM_TOTAL; i++ ) {
		if ( name == renderParmNames[i] ) {
			return static_cast<renderParm_t>( i );
		}
	}
	for ( int i = 0; i < NUM_USER_RENDERPARMS; i++ ) {
		if ( name == userParmNames[i] ) {
			return static_cast<renderParm_t>( RENDERPARM_USER + i );
		}
	}
	return std::nullopt;
}
