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
#ifndef __RENDERPROGSOURCE_H__
#define __RENDERPROGSOURCE_H__

#include <functional>
#include <optional>
#include <span>
#include <string_view>

#include "RenderParms.h"
#include "RenderBackendTypes.h"

/*
===============================================================================

	The source of a render program stage as every backend starts from it: base/renderprogs/
	<name>.vertex or .pixel in the engine's Cg dialect, its #include files expanded, PC defined
	and the dead code removed (StripDeadCode). OpenGL converts the text to GLSL when the packs
	have no GLSL for a stage; other backends translate it to their own language.

	renderProgDump <dir> writes this text and the parm list of every source, the reference for
	any change here (research/dx12/golden-strip holds the dumps of the code before it moved).

===============================================================================
*/

struct renderProgSource_t {
	idStr					text;		// includes expanded, PC defined, dead code removed
	idList<renderParm_t>	parms;		// every kept `uniform float4 rp*`, in declaration order (= the pack's .uniforms)
};

// The text of renderprogs/<path>, or nullopt.
using renderProgReader_t = std::function<std::optional<idStr>( std::string_view path )>;

// A VS_IN semantic of the renderprogs and the PC attribute it reads.
struct renderProgVertexInput_t {
	const char *	semantic;	// "POSITION", "TEXCOORD0", ...
	int				index;		// PC_ATTRIB_INDEX_*
};

// GL's StripDeadCode on text; name is the source's path for messages. #include lines are expanded
// in place through include before idParser runs. A text StripDeadCode cannot parse (or every text
// with r_skipStripDeadCode 1) comes back as it was, as GL compiled it. nullopt: a uniform no parm names.
[[nodiscard]] std::optional<renderProgSource_t>	R_StripRenderProgSource( std::string_view text, const char * name, const renderProgReader_t & include );
// renderprogs/<name without extension>.vertex|.pixel and its includes through fileSystem; nullopt when it is missing.
[[nodiscard]] std::optional<renderProgSource_t>	R_LoadRenderProgSource( const char * name, shaderStageType_t stage );
// renderprogs\<name without extension>.vertex|.pixel
[[nodiscard]] idStr								R_RenderProgPath( const char * name, shaderStageType_t stage );

[[nodiscard]] std::span<const renderProgVertexInput_t>	R_VertexInputs();
[[nodiscard]] int								R_VertexAttribIndex( std::string_view semantic );	// -1 if unknown
[[nodiscard]] const char *						R_RenderParmName( renderParm_t parm );				// "rpColor", "rpUser3"
[[nodiscard]] std::optional<renderParm_t>		R_FindRenderParm( std::string_view name );

#endif // !__RENDERPROGSOURCE_H__
