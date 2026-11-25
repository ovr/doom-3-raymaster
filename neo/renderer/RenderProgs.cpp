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

// renderProgInfo [file]: the console, or fs_savepath/<file>. The lines carry no handles or API
// names, so the files of two runs or two backends diff line by line.
static void R_RenderProgInfo_f( const idCmdArgs & args ) {
	if ( args.Argc() > 2 ) {
		// the console splits an unquoted name at '-', and the cut name would be written silently
		common->Printf( "usage: renderProgInfo [file] (quote a file name that has a '-')\n" );
		return;
	}
	const idStrList lines = renderProgManager.ProgramInfoLines();
	if ( args.Argc() < 2 ) {
		for ( int i = 0; i < lines.Num(); i++ ) {
			common->Printf( "%s\n", lines[i].c_str() );
		}
		return;
	}
	idStr text;
	for ( int i = 0; i < lines.Num(); i++ ) {
		text += lines[i];
		text += '\n';
	}
	if ( fileSystem->WriteFile( args.Argv( 1 ), text.c_str(), text.Length() ) <= 0 ) {
		common->Warning( "renderProgInfo: couldn't write %s", args.Argv( 1 ) );
		return;
	}
	common->Printf( "renderProgInfo: %d lines written to %s\n", lines.Num(), args.Argv( 1 ) );
}

/*
================================================================================================
idRenderProgManager::Init()
================================================================================================
*/
void idRenderProgManager::Init() {
	common->Printf( "----- Initializing Render Shaders -----\n" );

	builtinShaders.fill( -1 );
	// the vertex-only programs after the others, as the stage and program indexes always were; the gbuffer
	// ones last, so every other index is the same on each backend
	const bool gbuffer = ( renderBackend != nullptr ) && renderBackend->Caps().gbuffer;
	enum class phase_t { FRAGMENT, VERTEX_ONLY, GBUFFER };
	for ( const phase_t phase : { phase_t::FRAGMENT, phase_t::VERTEX_ONLY, phase_t::GBUFFER } ) {
		for ( const renderProgDef_t & program : R_RenderProgs() ) {
			const phase_t programPhase = program.gbuffer ? phase_t::GBUFFER : ( program.fragment ? phase_t::FRAGMENT : phase_t::VERTEX_ONLY );
			if ( program.builtin == renderProgBuiltin_t::NONE || programPhase != phase || ( program.gbuffer && !gbuffer ) ) {
				continue;
			}
			LoadBuiltin( program, false );
			if ( program.skinned != renderProgBuiltin_t::NONE ) {
				LoadBuiltin( program, true );
			}
		}
	}

	renderParms.SetNum( RENDERPARM_USER + MAX_GLSL_USER_PARMS, vec4_zero );

	cmdSystem->AddCommand( "reloadShaders", R_ReloadShaders, CMD_FL_RENDERER, "reloads shaders" );
	cmdSystem->AddCommand( "renderProgInfo", R_RenderProgInfo_f, CMD_FL_RENDERER, "lists what the render backend reported for every loaded program; renderProgInfo <file> writes it to fs_savepath" );
}

// A builtin's program has its vertex stage's index (BindShader takes it as the program). The fragment list
// lacks the vertex-only programs, so a builtin loaded after them has a smaller fragment index.
void idRenderProgManager::LoadBuiltin( const renderProgDef_t & program, bool skinned ) {
	const idStr name = R_RenderProgStageName( program, skinned );
	int & index = builtinShaders[static_cast<size_t>( skinned ? program.skinned : program.builtin )];
	if ( !program.fragment ) {
		index = FindVertexShader( name );
		[[maybe_unused]] const int programIndex = FindGLSLProgram( name, index, -1 );
		assert( programIndex == index );
		return;
	}
	index = vertexShaders.Append( NewVertexShader( name ) );
	fragmentShader_t fragment;
	fragment.name = name;
	const int fragmentIndex = fragmentShaders.Append( fragment );
	[[maybe_unused]] const int programIndex = renderProgs.Append( renderProg_t() );
	assert( programIndex == index );
	LoadVertexShader( index );
	LoadFragmentShader( fragmentIndex );
	LoadGLSLProgram( index, index, fragmentIndex );
}

// Its joint use from the program table: shared code binds the joints and rpEnableSkinning by it.
idRenderProgManager::vertexShader_t idRenderProgManager::NewVertexShader( const char * name ) {
	vertexShader_t shader;
	shader.name = name;
	if ( const std::optional<renderProgStage_t> found = R_FindRenderProg( name ) ) {
		shader.optionalSkinning = found->program->optionalSkinning;
		shader.usesJoints = found->skinned || shader.optionalSkinning;
	}
	return shader;
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

	for ( int i = 0; i < renderProgs.Num(); ++i ) {
		LoadGLSLProgram( i, renderProgs[i].vertexShaderIndex, renderProgs[i].fragmentShaderIndex );
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
	for ( int i = 0; i < renderProgs.Num(); ++i ) {
		if ( renderProgs[i].handle != programHandle_t::NONE ) {
			if ( callable ) {
				renderBackend->DestroyProgram( renderProgs[i].handle );
			}
			renderProgs[i].handle = programHandle_t::NONE;
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
	const int index = vertexShaders.Append( NewVertexShader( name ) );
	LoadVertexShader( index );
	currentVertexShader = index;
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

	for ( int i = 0; i < renderProgs.Num(); ++i ) {
		if ( ( renderProgs[i].vertexShaderIndex == vIndex ) && ( renderProgs[i].fragmentShaderIndex == fIndex ) ) {
			LoadGLSLProgram( i, vIndex, fIndex );
			return i;
		}
	}

	renderProg_t program;
	program.name = name;
	int index = renderProgs.Append( program );
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
	renderProg_t & prog = renderProgs[programIndex];

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
	if ( vIndex >= 0 && vIndex < renderProgs.Num() ) {
		currentRenderProgram = vIndex;
		RENDERLOG_PRINTF( "Binding GLSL Program %s\n", renderProgs[vIndex].name.c_str() );
		// a program without a handle leaves the previous one in use, as binding a bad OpenGL name did
		if ( renderProgs[vIndex].handle != programHandle_t::NONE ) {
			renderBackend->BindProgram( renderProgs[vIndex].handle );
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
		renderParms[rp][i] = value[i];
	}
}

/*
================================================================================================
idRenderProgManager::ZeroUniforms
================================================================================================
*/
void idRenderProgManager::ZeroUniforms() {
	memset( renderParms.Ptr(), 0, renderParms.Allocated() );
}

static idStr R_ParmList( const shaderStageInfo_t & info ) {
	if ( info.numParms == 0 ) {
		return "-";
	}
	idStr list;
	for ( int i = 0; i < info.numParms; i++ ) {
		list += va( ( i > 0 ) ? ",%d" : "%d", info.parms[i] );
	}
	return list;
}

idStrList idRenderProgManager::ProgramInfoLines() const {
	idStrList lines;
	lines.Append( va( "# renderProgInfo: %d programs, %d vertex and %d fragment stages; parms are renderParm_t indexes",
		renderProgs.Num(), vertexShaders.Num(), fragmentShaders.Num() ) );
	for ( int i = 0; i < renderProgs.Num(); i++ ) {
		const renderProg_t & prog = renderProgs[i];
		const vertexShader_t * vertex = ( prog.vertexShaderIndex >= 0 ) ? &vertexShaders[prog.vertexShaderIndex] : nullptr;
		const fragmentShader_t * fragment = ( prog.fragmentShaderIndex >= 0 ) ? &fragmentShaders[prog.fragmentShaderIndex] : nullptr;
		const programInfo_t & info = prog.info;
		lines.Append( va( "%d %s vs=%s fs=%s loaded=%d samplerMask=0x%04x cubeMask=0x%04x attribMask=0x%04x usesJoints=%d vsParms=%s fsParms=%s",
			i, prog.name.IsEmpty() ? "-" : prog.name.c_str(),
			( vertex != nullptr ) ? vertex->name.c_str() : "-", ( fragment != nullptr ) ? fragment->name.c_str() : "-",
			( prog.handle != programHandle_t::NONE ) ? 1 : 0, info.samplerMask, info.cubeMask, info.attribMask, info.usesJoints ? 1 : 0,
			( vertex != nullptr ) ? R_ParmList( vertex->info ).c_str() : "-", ( fragment != nullptr ) ? R_ParmList( fragment->info ).c_str() : "-" ) );
	}
	return lines;
}

