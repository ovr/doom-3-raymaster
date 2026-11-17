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

#include "RenderProgSource.h"

idCVar r_skipStripDeadCode( "r_skipStripDeadCode", "0", CVAR_BOOL, "Skip stripping dead code" );

namespace {

constexpr int MAX_INCLUDE_DEPTH = 16;

// the flags of the lexers idParser makes for the source and its includes
constexpr int SOURCE_LEXER_FLAGS = LEXFL_NOFATALERRORS;

struct idCGBlock {
	idStr prefix;	// tokens that comes before the name
	idStr name;		// the name
	idStr postfix;	// tokens that comes after the name
	bool used;		// whether or not this block is referenced anywhere
};

constexpr const char * types[] = {
	"int",
	"float",
	"half",
	"fixed",
	"bool",
	"cint",
	"cfloat",
	"void"
};

constexpr const char * typePosts[] = {
	"1", "2", "3", "4",
	"1x1", "1x2", "1x3", "1x4",
	"2x1", "2x2", "2x3", "2x4",
	"3x1", "3x2", "3x3", "3x4",
	"4x1", "4x2", "4x3", "4x4"
};

constexpr const char * prefixes[] = {
	"static",
	"const",
	"uniform",
	"struct",

	"sampler",

	"sampler1D",
	"sampler2D",
	"sampler3D",
	"samplerCUBE",

	"sampler1DShadow",		// GLSL
	"sampler2DShadow",		// GLSL
	"sampler3DShadow",		// GLSL
	"samplerCubeShadow",	// GLSL

	"sampler2DMS",			// GLSL
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
	"rpAlphaTest"
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

idStr ToStr( std::string_view text ) {
	idStr str;
	str.Append( text.data(), static_cast<int>( text.size() ) );
	return str;
}

std::optional<idStr> ReadTextFile( const char * path ) {
	void * buffer = nullptr;
	if ( fileSystem->ReadFile( path, &buffer ) <= 0 || buffer == nullptr ) {
		return std::nullopt;
	}
	idStr text( static_cast<const char *>( buffer ) );
	fileSystem->FreeFile( buffer );
	return text;
}

// idParser looked an include up next to its source, renderprogs/<path>
std::optional<idStr> ReadInclude( std::string_view path ) {
	idStr fileName = "renderprogs/";
	fileName += ToStr( path );
	return ReadTextFile( fileName );
}

struct tokenSpan_t {
	int		firstStart = 0;		// where the first token begins; 0 without tokens
	int		lastEnd = 0;		// where the last token ends
};

tokenSpan_t TokenSpan( const idStr & text, const char * name ) {
	idLexer lexer( SOURCE_LEXER_FLAGS | LEXFL_NOERRORS | LEXFL_NOWARNINGS );
	lexer.LoadMemory( text.c_str(), text.Length(), name );
	tokenSpan_t span;
	idToken token;
	for ( bool first = true; lexer.ReadToken( &token ); first = false ) {
		if ( first ) {
			span.firstStart = lexer.GetLastWhiteSpaceEnd();
		}
		span.lastEnd = lexer.GetFileOffset();
	}
	return span;
}

// Every #include "file" the reader has becomes the file's text, cut so that idParser reads the
// tokens and line breaks it read from the pushed script: the file's own leading whitespace, and
// one line break more before the next token of the includer. Others stay for idParser to find.
idStr ExpandIncludes( const idStr & text, const char * name, const renderProgReader_t & include, int depth ) {
	idLexer lexer( SOURCE_LEXER_FLAGS | LEXFL_NOERRORS | LEXFL_NOWARNINGS );
	lexer.LoadMemory( text.c_str(), text.Length(), name );
	idStr out;
	int copied = 0;
	idToken token;
	while ( lexer.ReadToken( &token ) ) {
		if ( token.type != TT_PUNCTUATION || token != "#" ) {
			continue;
		}
		const int directiveStart = lexer.GetLastWhiteSpaceStart();	// the end of the token before '#'
		idToken keyword;
		if ( !lexer.ReadToken( &keyword ) ) {
			break;
		}
		if ( keyword.linesCrossed > 0 || keyword != "include" ) {
			lexer.UnreadToken( &keyword );
			continue;
		}
		idToken file;
		if ( !lexer.ReadToken( &file ) ) {
			break;
		}
		if ( file.linesCrossed > 0 || file.type != TT_STRING ) {
			lexer.UnreadToken( &file );
			continue;
		}
		const int directiveEnd = lexer.GetFileOffset();
		const std::optional<idStr> included = ( depth < MAX_INCLUDE_DEPTH ) ? include( file.c_str() ) : std::nullopt;
		if ( !included ) {
			continue;
		}
		const idStr expanded = ExpandIncludes( *included, file.c_str(), include, depth + 1 );
		const tokenSpan_t span = TokenSpan( expanded, file.c_str() );

		out.Append( text.c_str() + copied, directiveStart - copied );
		if ( span.firstStart == 0 && directiveStart > 0 ) {
			out += ' ';		// two scripts never merge their tokens
		}
		out.Append( expanded.c_str(), span.lastEnd );
		out += '\n';
		copied = directiveEnd;
	}
	out.Append( text.c_str() + copied, text.Length() - copied );
	return out;
}

// GL's StripDeadCode: nullopt when the text has a construct it does not know.
std::optional<idStr> StripDeadCode( const idStr & in, const char * name ) {
	idParser src( LEXFL_NOFATALERRORS );
	src.LoadMemory( in.c_str(), in.Length(), name );
	src.AddDefine( "PC" );

	idList< idCGBlock, TAG_RENDERPROG > blocks;

	blocks.SetNum( 100 );

	idToken token;
	while ( !src.EndOfFile() ) {
		idCGBlock & block = blocks.Alloc();
		// read prefix
		while ( src.ReadToken( &token ) ) {
			bool found = false;
			for ( const char * prefix : prefixes ) {
				if ( token == prefix ) {
					found = true;
					break;
				}
			}
			if ( !found ) {
				for ( const char * type : types ) {
					if ( token == type ) {
						found = true;
						break;
					}
					const int typeLen = idStr::Length( type );
					if ( token.Cmpn( type, typeLen ) == 0 ) {
						for ( const char * typePost : typePosts ) {
							if ( idStr::Cmp( token.c_str() + typeLen, typePost ) == 0 ) {
								found = true;
								break;
							}
						}
						if ( found ) {
							break;
						}
					}
				}
			}
			if ( found ) {
				if ( block.prefix.Length() > 0 && token.WhiteSpaceBeforeToken() ) {
					block.prefix += ' ';
				}
				block.prefix += token;
			} else {
				src.UnreadToken( &token );
				break;
			}
		}
		if ( !src.ReadToken( &token ) ) {
			blocks.SetNum( blocks.Num() - 1 );
			break;
		}
		block.name = token;

		if ( src.PeekTokenString( "=" ) || src.PeekTokenString( ":" ) || src.PeekTokenString( "[" ) ) {
			src.ReadToken( &token );
			block.postfix = token;
			while ( src.ReadToken( &token ) ) {
				if ( token == ";" ) {
					block.postfix += ';';
					break;
				} else {
					if ( token.WhiteSpaceBeforeToken() ){
						block.postfix += ' ';
					}
					block.postfix += token;
				}
			}
		} else if ( src.PeekTokenString( "(" ) ) {
			idStr parms, body;
			src.ParseBracedSection( parms, -1, true, '(', ')' );
			if ( src.CheckTokenString( ";" ) ) {
				block.postfix = parms + ";";
			} else {
				src.ParseBracedSection( body, -1, true, '{', '}' );
				block.postfix = parms + " " + body;
			}
		} else if ( src.PeekTokenString( "{" ) ) {
			src.ParseBracedSection( block.postfix, -1, true, '{', '}' );
			if ( src.CheckTokenString( ";" ) ) {
				block.postfix += ';';
			}
		} else if ( src.CheckTokenString( ";" ) ) {
			block.postfix = idStr( ';' );
		} else {
			src.Warning( "Could not strip dead code -- unknown token %s\n", token.c_str() );
			return std::nullopt;
		}
	}

	idList<int, TAG_RENDERPROG> stack;
	for ( int i = 0; i < blocks.Num(); i++ ) {
		blocks[i].used = ( ( blocks[i].name == "main" )
			|| blocks[i].name.Right( 4 ) == "_ubo"
			);

		if ( blocks[i].name == "include" ) {
			blocks[i].used = true;
			blocks[i].name = ""; // clear out the include tag
		}

		if ( blocks[i].used ) {
			stack.Append( i );
		}
	}

	while ( stack.Num() > 0 ) {
		int i = stack[stack.Num() - 1];
		stack.SetNum( stack.Num() - 1 );

		idLexer blockSrc( LEXFL_NOFATALERRORS );
		blockSrc.LoadMemory( blocks[i].postfix.c_str(), blocks[i].postfix.Length(), name );
		while ( blockSrc.ReadToken( &token ) ) {
			for ( int j = 0; j < blocks.Num(); j++ ) {
				if ( !blocks[j].used ) {
					if ( token == blocks[j].name ) {
						blocks[j].used = true;
						stack.Append( j );
					}
				}
			}
		}
	}

	idStr out;

	for ( int i = 0; i < blocks.Num(); i++ ) {
		if ( blocks[i].used ) {
			out += blocks[i].prefix;
			out += ' ';
			out += blocks[i].name;
			out += ' ';
			out += blocks[i].postfix;
			out += '\n';
		}
	}

	return out;
}

// What GL's converter collects from the same text: every `uniform float4 <name>` outside the
// parameters of main, in order.
std::optional<idList<renderParm_t>> UniformParms( const idStr & text, const char * name ) {
	idLexer src( LEXFL_NOFATALERRORS );
	src.LoadMemory( text.c_str(), text.Length(), name );
	idList<renderParm_t> parms;
	idToken token;
	while ( src.ReadToken( &token ) ) {
		if ( token == "void" && src.CheckTokenString( "main" ) ) {
			src.SkipUntilString( ")" );
			continue;
		}
		if ( token != "uniform" || !src.CheckTokenString( "float4" ) || !src.ReadToken( &token ) ) {
			continue;
		}
		const std::optional<renderParm_t> parm = R_FindRenderParm( token.c_str() );
		if ( !parm ) {
			common->Warning( "%s: uniform %s is no render parm", name, token.c_str() );
			return std::nullopt;
		}
		parms.Append( *parm );
	}
	return parms;
}

}	// namespace

std::optional<renderProgSource_t> R_StripRenderProgSource( std::string_view text, const char * name, const renderProgReader_t & include ) {
	const idStr original = ToStr( text );
	std::optional<idStr> stripped;
	if ( !r_skipStripDeadCode.GetBool() ) {
		stripped = StripDeadCode( ExpandIncludes( original, name, include, 0 ), name );
	}
	renderProgSource_t source;
	source.text = stripped ? *stripped : original;
	std::optional<idList<renderParm_t>> parms = UniformParms( source.text, name );
	if ( !parms ) {
		return std::nullopt;
	}
	source.parms = std::move( *parms );
	return source;
}

idStr R_RenderProgPath( const char * name, shaderStageType_t stage ) {
	idStr path;
	path.Format( "renderprogs\\%s", name );
	path.StripFileExtension();
	path += ( stage == shaderStageType_t::FRAGMENT ) ? ".pixel" : ".vertex";
	return path;
}

std::optional<renderProgSource_t> R_LoadRenderProgSource( const char * name, shaderStageType_t stage ) {
	const idStr path = R_RenderProgPath( name, stage );
	const std::optional<idStr> text = ReadTextFile( path );
	if ( !text ) {
		return std::nullopt;
	}
	return R_StripRenderProgSource( text->c_str(), path, ReadInclude );
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

// The format is fixed: research/dx12/golden-strip holds the files of the code before StripDeadCode moved here.
CONSOLE_COMMAND_SHIP( renderProgDump, "writes StripDeadCode and the parm list of every renderprogs/*.vertex and *.pixel to fs_savepath/<dir>", nullptr ) {
	if ( args.Argc() != 2 ) {
		common->Printf( "usage: renderProgDump <dir> (quote a dir name that has a '-')\n" );
		return;
	}
	const idStr dir = args.Argv( 1 );
	int written = 0;
	for ( const char * extension : { ".vertex", ".pixel" } ) {
		idFileList * files = fileSystem->ListFiles( "renderprogs", extension, true );
		for ( int i = 0; i < files->GetNumFiles(); i++ ) {
			const char * fileName = files->GetFile( i );
			idStr path;
			path.Format( "renderprogs/%s", fileName );
			const std::optional<idStr> text = ReadTextFile( path );
			if ( !text ) {
				common->Warning( "renderProgDump: can't read %s", path.c_str() );
				continue;
			}
			path.Format( "renderprogs\\%s", fileName );
			const std::optional<renderProgSource_t> source = R_StripRenderProgSource( text->c_str(), path, ReadInclude );
			if ( !source ) {
				continue;
			}
			idStr dump = va( "// renderprogs/%s after StripDeadCode, %d parms in constant order\n", fileName, source->parms.Num() );
			for ( int p = 0; p < source->parms.Num(); p++ ) {
				dump += va( "// parm %d %s\n", static_cast<int>( source->parms[p] ), R_RenderParmName( source->parms[p] ) );
			}
			dump += source->text;
			if ( fileSystem->WriteFile( va( "%s/%s.txt", dir.c_str(), fileName ), dump.c_str(), dump.Length() ) > 0 ) {
				written++;
			}
		}
		fileSystem->FreeFileList( files );
	}
	common->Printf( "renderProgDump: %d sources written to %s\n", written, dir.c_str() );
}
