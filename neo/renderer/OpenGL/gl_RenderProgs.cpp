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
#include "../../idlib/precompiled.h"
#include "qgl.h"

#include "../tr_local.h"

#include <string>
#include <string_view>
#include "../RenderProgTable.h"
#include "gl_Backend.h"

#define VERTEX_UNIFORM_ARRAY_NAME				"_va_"
#define FRAGMENT_UNIFORM_ARRAY_NAME				"_fa_"

// the GLSL input of a PC vertex attribute (R_VertexInputs has the semantics)
static const char * GL_VertexInputName( int attribIndex ) {
	switch ( attribIndex ) {
		case PC_ATTRIB_INDEX_VERTEX:	return "in_Position";
		case PC_ATTRIB_INDEX_ST:		return "in_TexCoord";
		case PC_ATTRIB_INDEX_NORMAL:	return "in_Normal";
		case PC_ATTRIB_INDEX_TANGENT:	return "in_Tangent";
		case PC_ATTRIB_INDEX_COLOR:		return "in_Color";
		case PC_ATTRIB_INDEX_COLOR2:	return "in_Color2";
		default:						return nullptr;
	}
}

/*
================================================================================================
GL_NewShaderStage
================================================================================================
*/
static shaderHandle_t GL_NewShaderStage( GLuint shader, const char * name, const shaderStageInfo_t & info ) {
	glShaderStage_t * stage = new glShaderStage_t;
	stage->shader = shader;
	stage->info = info;
	stage->name = name;
	return static_cast<shaderHandle_t>( reinterpret_cast<uintptr_t>( stage ) );
}

/*
========================
GL_StripBuiltinDeclarations

The GLSL baked into the retail packs declares built-ins ("in vec4 gl_Color;") under "#version 150".
That is an error which a driver's profile for the game's exe name lets through, and no other exe.
The lines are blanked, so the line numbers of compile errors stay.
========================
*/
static void GL_StripBuiltinDeclarations( idStr & glsl ) {
	// the baked files end their lines with CR LF
	constexpr std::string_view coreVersion = "#version 150";
	constexpr std::string_view compatibility = " compatibility";

	const std::string_view text( glsl.c_str(), glsl.Length() );
	std::string out;
	out.reserve( text.size() + compatibility.size() );

	size_t lineStart = 0;
	if ( text.starts_with( coreVersion ) && text.size() > coreVersion.size() && ( text[coreVersion.size()] == '\r' || text[coreVersion.size()] == '\n' ) ) {
		out += coreVersion;
		out += compatibility;
		lineStart = coreVersion.size();
	}
	while ( lineStart < text.size() ) {
		size_t lineEnd = text.find( '\n', lineStart );
		lineEnd = ( lineEnd == std::string_view::npos ) ? text.size() : lineEnd + 1;
		const std::string_view line = text.substr( lineStart, lineEnd - lineStart );
		lineStart = lineEnd;

		// "in <type> gl_Name;" or "out <type> gl_Name;"
		const bool inOut = line.starts_with( "in " ) || line.starts_with( "out " );
		const size_t name = line.rfind( ' ' );
		if ( inOut && name != std::string_view::npos && line.substr( name + 1 ).starts_with( "gl_" ) ) {
			out += line.ends_with( "\r\n" ) ? "\r\n" : "\n";
			continue;
		}
		out += line;
	}
	glsl = out.c_str();
}

/*
================================================================================================
idRenderBackendGL::CreateShaderStage

Loads renderprogs/glsl/<name>_vertex.glsl or _fragment.glsl with its .uniforms list as id baked them
into the resource files; a loose file is never read. A stage the packs lack is a record without a
shader, not NONE: shared code never looks for it again and its program fails to link.
================================================================================================
*/
shaderHandle_t idRenderBackendGL::CreateShaderStage( shaderStageType_t stage, const char * name, shaderStageInfo_t & info ) {
	const GLenum target = ( stage == shaderStageType_t::FRAGMENT ) ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER;
	info.numParms = 0;

	idStr file;
	file.Format( "renderprogs\\glsl\\%s", name );
	file.StripFileExtension();
	file += ( target == GL_FRAGMENT_SHADER ) ? "_fragment" : "_vertex";
	const idStr glslFile = file + ".glsl";
	const idStr uniformsFile = file + ".uniforms";

	std::optional<idStr> baked = fileSystem->ReadResourceText( glslFile );
	if ( !baked || baked->IsEmpty() ) {
		return GL_NewShaderStage( 0, name, info );
	}
	idStr programGLSL = std::move( *baked );
	const std::optional<idStr> uniforms = fileSystem->ReadResourceText( uniformsFile );
	if ( !uniforms || uniforms->IsEmpty() ) {
		idLib::Error( "uniform file %s could not be loaded and may be corrupt", uniformsFile.c_str() );
	}
	const idStr & programUniforms = *uniforms;

	GL_StripBuiltinDeclarations( programGLSL );

	// find the uniforms locations in either the vertex or fragment uniform array
	idLexer src( programUniforms, programUniforms.Length(), "uniforms" );
	idToken token;
	while ( src.ReadToken( &token ) ) {
		const std::optional<renderParm_t> parm = R_FindRenderParm( token.c_str() );
		if ( !parm ) {
			idLib::Error( "couldn't find uniform %s for %s", token.c_str(), glslFile.c_str() );
		}
		if ( info.numParms == MAX_RENDER_PARMS ) {
			idLib::Error( "more than %d uniforms for %s", MAX_RENDER_PARMS, glslFile.c_str() );
		}
		info.parms[info.numParms++] = static_cast<uint8>( *parm );
	}

	// create and compile the shader
	const GLuint shader = qglCreateShader( target );
	if ( shader ) {
		const char * source[1] = { programGLSL.c_str() };

		qglShaderSource( shader, 1, source, NULL );
		qglCompileShader( shader );

		int infologLength = 0;
		qglGetShaderiv( shader, GL_INFO_LOG_LENGTH, &infologLength );
		if ( infologLength > 1 ) {
			idTempArray<char> infoLog( infologLength );
			int charsWritten = 0;
			qglGetShaderInfoLog( shader, infologLength, &charsWritten, infoLog.Ptr() );

			// catch the strings the ATI and Intel drivers output on success
			if ( strstr( infoLog.Ptr(), "successfully compiled to run on hardware" ) != NULL ||
					strstr( infoLog.Ptr(), "No errors." ) != NULL ) {
				//idLib::Printf( "%s program %s from %s compiled to run on hardware\n", typeName, GetName(), GetFileName() );
			} else {
				idLib::Printf( "While compiling %s program %s\n", ( target == GL_FRAGMENT_SHADER ) ? "fragment" : "vertex" , glslFile.c_str() );

				const char separator = '\n';
				idList<idStr> lines;
				lines.Clear();
				lines.Append( programGLSL );
				for ( int index = 0, ofs = lines[index].Find( separator ); ofs != -1; index++, ofs = lines[index].Find( separator ) ) {
					lines.Append( lines[index].c_str() + ofs + 1 );
					lines[index].CapLength( ofs );
				}

				idLib::Printf( "-----------------\n" );
				for ( int i = 0; i < lines.Num(); i++ ) {
					idLib::Printf( "%3d: %s\n", i+1, lines[i].c_str() );
				}
				idLib::Printf( "-----------------\n" );

				idLib::Printf( "%s\n", infoLog.Ptr() );
			}
		}

		GLint compiled = GL_FALSE;
		qglGetShaderiv( shader, GL_COMPILE_STATUS, &compiled );
		if ( compiled == GL_FALSE ) {
			qglDeleteShader( shader );
			return shaderHandle_t::NONE;
		}
	}

	return GL_NewShaderStage( shader, name, info );
}

/*
================================================================================================
idRenderBackendGL::DestroyShaderStage
================================================================================================
*/
void idRenderBackendGL::DestroyShaderStage( shaderHandle_t shader ) {
	glShaderStage_t * stage = GL_ShaderStage( shader );
	if ( stage == nullptr ) {
		return;
	}
	qglDeleteShader( stage->shader );
	delete stage;
}

/*
================================================================================================
idRenderBackendGL::CreateProgram

Leaves the new program in use without telling the latch, as the code before it did. A failed
link is an error: it ends the map load that parsed the material.
================================================================================================
*/
programHandle_t idRenderBackendGL::CreateProgram( shaderHandle_t vertex, shaderHandle_t fragment, const char * debugName, programInfo_t & info ) {
	const glShaderStage_t * vertexStage = GL_ShaderStage( vertex );
	const glShaderStage_t * fragmentStage = GL_ShaderStage( fragment );
	const char * vertexName = ( vertexStage != nullptr ) ? vertexStage->name.c_str() : "<Invalid>";
	const char * fragmentName = ( fragmentStage != nullptr ) ? fragmentStage->name.c_str() : "<Invalid>";

	const GLuint program = qglCreateProgram();
	if ( program ) {

		if ( vertexStage != nullptr ) {
			qglAttachShader( program, vertexStage->shader );
		}

		if ( fragmentStage != nullptr ) {
			qglAttachShader( program, fragmentStage->shader );
		}

		// bind vertex attribute locations
		for ( const renderProgVertexInput_t & input : R_VertexInputs() ) {
			qglBindAttribLocation( program, input.index, GL_VertexInputName( input.index ) );
		}

		qglLinkProgram( program );

		int infologLength = 0;
		qglGetProgramiv( program, GL_INFO_LOG_LENGTH, &infologLength );
		if ( infologLength > 1 ) {
			char * infoLog = (char *)malloc( infologLength );
			int charsWritten = 0;
			qglGetProgramInfoLog( program, infologLength, &charsWritten, infoLog );

			// catch the strings the ATI and Intel drivers output on success
			if ( strstr( infoLog, "Vertex shader(s) linked, fragment shader(s) linked." ) != NULL || strstr( infoLog, "No errors." ) != NULL ) {
				//idLib::Printf( "render prog %s from %s linked\n", GetName(), GetFileName() );
			} else {
				idLib::Printf( "While linking GLSL program %s with vertexShader %s and fragmentShader %s\n",
					debugName, vertexName, fragmentName );
				idLib::Printf( "%s\n", infoLog );
			}

			free( infoLog );
		}
	}

	int linked = GL_FALSE;
	qglGetProgramiv( program, GL_LINK_STATUS, &linked );
	if ( linked == GL_FALSE ) {
		qglDeleteProgram( program );
		idLib::Error( "While linking GLSL program %s with vertexShader %s and fragmentShader %s\n",
			debugName, vertexName, fragmentName );
	}

	glProgram_t * prog = new glProgram_t;
	prog->progId = program;
	prog->vertexInfo = ( vertexStage != nullptr ) ? vertexStage->info : shaderStageInfo_t {};
	prog->fragmentInfo = ( fragmentStage != nullptr ) ? fragmentStage->info : shaderStageInfo_t {};

	prog->vertexUniformArray = qglGetUniformLocation( program, VERTEX_UNIFORM_ARRAY_NAME );
	prog->fragmentUniformArray = qglGetUniformLocation( program, FRAGMENT_UNIFORM_ARRAY_NAME );

	assert( prog->vertexUniformArray != -1 || prog->vertexInfo.numParms == 0 );
	assert( prog->fragmentUniformArray != -1 || prog->fragmentInfo.numParms == 0 );

	// get the uniform buffer binding for skinning joint matrices
	GLint blockIndex = qglGetUniformBlockIndex( program, "matrices_ubo" );
	if ( blockIndex != -1 ) {
		qglUniformBlockBinding( program, blockIndex, 0 );
	}

	// set the texture unit locations once for the render program. We only need to do this once since we only link the program once
	info = {};
	qglUseProgram( program );
	for ( int i = 0; i < MAX_PROG_TEXTURE_PARMS; ++i ) {
		GLint loc = qglGetUniformLocation( program, va( "samp%d", i ) );
		if ( loc != -1 ) {
			qglUniform1i( loc, i );
			info.samplerMask = static_cast<uint16>( info.samplerMask | ( 1 << i ) );
		}
	}

	// the rest of what the program needs from a draw: queries only, they change nothing in GL
	info.usesJoints = ( blockIndex != -1 );
	for ( const renderProgVertexInput_t & input : R_VertexInputs() ) {
		if ( qglGetAttribLocation( program, GL_VertexInputName( input.index ) ) != -1 ) {
			info.attribMask = static_cast<uint16>( info.attribMask | ( 1 << input.index ) );
		}
	}
	GLint numUniforms = 0;
	qglGetProgramiv( program, GL_ACTIVE_UNIFORMS, &numUniforms );
	for ( int i = 0; i < numUniforms; i++ ) {
		char uniformName[64] = {};
		GLint size = 0;
		GLenum type = 0;
		qglGetActiveUniform( program, i, sizeof( uniformName ), NULL, &size, &type, uniformName );
		if ( type == GL_SAMPLER_CUBE && idStr::Cmpn( uniformName, "samp", 4 ) == 0 ) {
			const int slot = atoi( uniformName + 4 );
			if ( slot >= 0 && slot < MAX_PROG_TEXTURE_PARMS ) {
				info.cubeMask = static_cast<uint16>( info.cubeMask | ( 1 << slot ) );
			}
		}
	}

#ifdef _DEBUG
	prog->info = info;
	prog->name = debugName;
	prog->warnedSamplers = 0;
	prog->warnedLayouts = 0;
	prog->warnedJoints = false;
#endif

	return static_cast<programHandle_t>( reinterpret_cast<uintptr_t>( prog ) );
}

/*
================================================================================================
idRenderBackendGL::DestroyProgram
================================================================================================
*/
void idRenderBackendGL::DestroyProgram( programHandle_t program ) {
	glProgram_t * prog = GL_Program( program );
	if ( prog == nullptr ) {
		return;
	}
	if ( latch.program == program ) {
		latch.program = programHandle_t::NONE;
	}
	qglDeleteProgram( prog->progId );
	delete prog;
}

/*
================================================================================================
idRenderBackendGL::API_BindProgram
================================================================================================
*/
void idRenderBackendGL::API_BindProgram( programHandle_t program ) {
	qglUseProgram( ( program != programHandle_t::NONE ) ? GL_Program( program )->progId : 0 );
}

/*
================================================================================================
idRenderBackendGL::CommitParms

The uniforms go to the program GL has in use, which is not this one right after another
program was created.
================================================================================================
*/
void idRenderBackendGL::CommitParms( programHandle_t program, const idVec4 * parms ) {
	const glProgram_t & prog = *GL_Program( program );

	ALIGNTYPE16 idVec4 localVectors[MAX_RENDER_PARMS];

	if ( prog.vertexUniformArray != -1 && prog.vertexInfo.numParms > 0 ) {
		const int num = R_PackRenderParms( localVectors, parms, prog.vertexInfo );
		qglUniform4fv( prog.vertexUniformArray, num, localVectors->ToFloatPtr() );
	}

	if ( prog.fragmentUniformArray != -1 && prog.fragmentInfo.numParms > 0 ) {
		const int num = R_PackRenderParms( localVectors, parms, prog.fragmentInfo );
		qglUniform4fv( prog.fragmentUniformArray, num, localVectors->ToFloatPtr() );
	}
}

