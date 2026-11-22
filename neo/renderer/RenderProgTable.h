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
#ifndef __RENDERPROGTABLE_H__
#define __RENDERPROGTABLE_H__

#include <optional>
#include <span>
#include <string_view>

#include "RenderParms.h"
#include "RenderBackendTypes.h"

/*
	The render programs the engine draws with. A program <name> has a vertex stage and, unless it only
	writes depth, a fragment stage; shared code names both "<name>.vfp" (".vp" without a fragment
	stage), and "<name>_skinned.vfp" is its skinned variant. The backend loads a stage by that name
	from base/shaders/<name>.hlsl, VSMain and PSMain; the skinned variant with SKINNED=1.

	Materials name programs too ("heatHaze.vfp"), case ignored. A name the table lacks, or a stage it
	does not list, is a stage without a shader, and its program fails to link as it did in GL.
*/

enum class renderProgBuiltin_t : uint8 {
	GUI,
	COLOR,
	SIMPLESHADE,
	TEXTURED,
	TEXTURE_VERTEXCOLOR,
	TEXTURE_VERTEXCOLOR_SKINNED,
	TEXTURE_TEXGEN_VERTEXCOLOR,
	TEXTURE_VERTEXCOLOR_SOFT,
	INTERACTION,
	INTERACTION_SKINNED,
	INTERACTION_AMBIENT,
	INTERACTION_AMBIENT_SKINNED,
	ENVIRONMENT,
	ENVIRONMENT_SKINNED,
	BUMPY_ENVIRONMENT,
	BUMPY_ENVIRONMENT_SKINNED,

	DEPTH,
	DEPTH_SKINNED,
	SHADOW,
	SHADOW_SKINNED,
	SHADOW_DEBUG,
	SHADOW_DEBUG_SKINNED,

	BLENDLIGHT,
	FOG,
	FOG_SKINNED,
	SKYBOX,
	WOBBLESKY,
	POSTPROCESS,
	STEREO_DEGHOST,
	STEREO_WARP,
	ZCULL_RECONSTRUCT,
	BINK,
	BINK_GUI,
	STEREO_INTERLACE,
	MOTION_BLUR,

	COUNT,
	NONE = 0xFF
};

struct renderProgDef_t {
	const char *		name;										// "interaction": the file stem
	renderProgBuiltin_t	builtin = renderProgBuiltin_t::NONE;		// NONE: a program only materials name
	renderProgBuiltin_t	skinned = renderProgBuiltin_t::NONE;		// <name>_skinned; NONE: no skinned variant
	bool				fragment = true;							// false: vertex only (shadow volumes)
	bool				optionalSkinning = false;					// skins only when rpEnableSkinning is set
};

// A stage name of the table: its program and whether it is the skinned variant.
struct renderProgStage_t {
	const renderProgDef_t *	program;
	bool					skinned;
};

[[nodiscard]] std::span<const renderProgDef_t>	R_RenderProgs();
// "interaction_skinned.vfp" -> interaction, skinned; nullopt: not a program of the table
[[nodiscard]] std::optional<renderProgStage_t>	R_FindRenderProg( std::string_view stageName );
// "interaction.vfp", "shadow_skinned.vp"
[[nodiscard]] idStr								R_RenderProgStageName( const renderProgDef_t & program, bool skinned );
// whether the table lists this stage of the program a stage name belongs to
[[nodiscard]] bool								R_RenderProgHasStage( std::string_view stageName, shaderStageType_t stage );

// A VS_IN semantic of the render programs and the PC attribute it reads.
struct renderProgVertexInput_t {
	const char *	semantic;	// "POSITION", "TEXCOORD0", ...
	int				index;		// PC_ATTRIB_INDEX_*
};

[[nodiscard]] std::span<const renderProgVertexInput_t>	R_VertexInputs();
[[nodiscard]] int								R_VertexAttribIndex( std::string_view semantic );	// -1 if unknown
[[nodiscard]] const char *						R_RenderParmName( renderParm_t parm );				// "rpColor", "rpUser3"
[[nodiscard]] std::optional<renderParm_t>		R_FindRenderParm( std::string_view name );

#endif // !__RENDERPROGTABLE_H__
