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
#include "../RenderProgSource.h"
#include "gl_Backend.h"

#define VERTEX_UNIFORM_ARRAY_NAME				"_va_"
#define FRAGMENT_UNIFORM_ARRAY_NAME				"_fa_"

static const int AT_VS_IN  = BIT( 1 );
static const int AT_VS_OUT = BIT( 2 );
static const int AT_PS_IN  = BIT( 3 );
static const int AT_PS_OUT = BIT( 4 );

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

// the GLSL names of the semantics of the other in and out structs
struct attribInfo_t {
	const char *	type;
	const char *	name;
	const char *	semantic;
	const char *	glsl;
	int				flags;
};

static constexpr attribInfo_t attribsPC[] = {
	// pre-defined vertex program output
	{ "float4",		"position",		"POSITION",		"gl_Position",			AT_VS_OUT },
	{ "float",		"clip0",		"CLP0",			"gl_ClipDistance[0]",	AT_VS_OUT },
	{ "float",		"clip1",		"CLP1",			"gl_ClipDistance[1]",	AT_VS_OUT },
	{ "float",		"clip2",		"CLP2",			"gl_ClipDistance[2]",	AT_VS_OUT },
	{ "float",		"clip3",		"CLP3",			"gl_ClipDistance[3]",	AT_VS_OUT },
	{ "float",		"clip4",		"CLP4",			"gl_ClipDistance[4]",	AT_VS_OUT },
	{ "float",		"clip5",		"CLP5",			"gl_ClipDistance[5]",	AT_VS_OUT },

	// pre-defined fragment program input
	{ "float4",		"position",		"WPOS",			"gl_FragCoord",			AT_PS_IN },
	{ "half4",		"hposition",	"WPOS",			"gl_FragCoord",			AT_PS_IN },
	{ "float",		"facing",		"FACE",			"gl_FrontFacing",		AT_PS_IN },

	// fragment program output
	{ "float4",		"color",		"COLOR",		"gl_FragColor",		AT_PS_OUT }, // GLSL version 1.2 doesn't allow for custom color name mappings
	{ "half4",		"hcolor",		"COLOR",		"gl_FragColor",		AT_PS_OUT },
	{ "float4",		"color0",		"COLOR0",		"gl_FragColor",		AT_PS_OUT },
	{ "float4",		"color1",		"COLOR1",		"gl_FragColor",		AT_PS_OUT },
	{ "float4",		"color2",		"COLOR2",		"gl_FragColor",		AT_PS_OUT },
	{ "float4",		"color3",		"COLOR3",		"gl_FragColor",		AT_PS_OUT },
	{ "float",		"depth",		"DEPTH",		"gl_FragDepth",		AT_PS_OUT },

	// vertex to fragment program pass through
	{ "float4",		"color",		"COLOR",		"gl_FrontColor",			AT_VS_OUT },
	{ "float4",		"color0",		"COLOR0",		"gl_FrontColor",			AT_VS_OUT },
	{ "float4",		"color1",		"COLOR1",		"gl_FrontSecondaryColor",	AT_VS_OUT },


	{ "float4",		"color",		"COLOR",		"gl_Color",				AT_PS_IN },
	{ "float4",		"color0",		"COLOR0",		"gl_Color",				AT_PS_IN },
	{ "float4",		"color1",		"COLOR1",		"gl_SecondaryColor",	AT_PS_IN },

	{ "half4",		"hcolor",		"COLOR",		"gl_Color",				AT_PS_IN },
	{ "half4",		"hcolor0",		"COLOR0",		"gl_Color",				AT_PS_IN },
	{ "half4",		"hcolor1",		"COLOR1",		"gl_SecondaryColor",	AT_PS_IN },

	{ "float4",		"texcoord0",	"TEXCOORD0_centroid",	"vofi_TexCoord0",	AT_PS_IN },
	{ "float4",		"texcoord1",	"TEXCOORD1_centroid",	"vofi_TexCoord1",	AT_PS_IN },
	{ "float4",		"texcoord2",	"TEXCOORD2_centroid",	"vofi_TexCoord2",	AT_PS_IN },
	{ "float4",		"texcoord3",	"TEXCOORD3_centroid",	"vofi_TexCoord3",	AT_PS_IN },
	{ "float4",		"texcoord4",	"TEXCOORD4_centroid",	"vofi_TexCoord4",	AT_PS_IN },
	{ "float4",		"texcoord5",	"TEXCOORD5_centroid",	"vofi_TexCoord5",	AT_PS_IN },
	{ "float4",		"texcoord6",	"TEXCOORD6_centroid",	"vofi_TexCoord6",	AT_PS_IN },
	{ "float4",		"texcoord7",	"TEXCOORD7_centroid",	"vofi_TexCoord7",	AT_PS_IN },
	{ "float4",		"texcoord8",	"TEXCOORD8_centroid",	"vofi_TexCoord8",	AT_PS_IN },
	{ "float4",		"texcoord9",	"TEXCOORD9_centroid",	"vofi_TexCoord9",	AT_PS_IN },

	{ "float4",		"texcoord0",	"TEXCOORD0",	"vofi_TexCoord0",		AT_VS_OUT | AT_PS_IN },
	{ "float4",		"texcoord1",	"TEXCOORD1",	"vofi_TexCoord1",		AT_VS_OUT | AT_PS_IN },
	{ "float4",		"texcoord2",	"TEXCOORD2",	"vofi_TexCoord2",		AT_VS_OUT | AT_PS_IN },
	{ "float4",		"texcoord3",	"TEXCOORD3",	"vofi_TexCoord3",		AT_VS_OUT | AT_PS_IN },
	{ "float4",		"texcoord4",	"TEXCOORD4",	"vofi_TexCoord4",		AT_VS_OUT | AT_PS_IN },
	{ "float4",		"texcoord5",	"TEXCOORD5",	"vofi_TexCoord5",		AT_VS_OUT | AT_PS_IN },
	{ "float4",		"texcoord6",	"TEXCOORD6",	"vofi_TexCoord6",		AT_VS_OUT | AT_PS_IN },
	{ "float4",		"texcoord7",	"TEXCOORD7",	"vofi_TexCoord7",		AT_VS_OUT | AT_PS_IN },
	{ "float4",		"texcoord8",	"TEXCOORD8",	"vofi_TexCoord8",		AT_VS_OUT | AT_PS_IN },
	{ "float4",		"texcoord9",	"TEXCOORD9",	"vofi_TexCoord9",		AT_VS_OUT | AT_PS_IN },

	{ "half4",		"htexcoord0",	"TEXCOORD0",	"vofi_TexCoord0",		AT_PS_IN },
	{ "half4",		"htexcoord1",	"TEXCOORD1",	"vofi_TexCoord1",		AT_PS_IN },
	{ "half4",		"htexcoord2",	"TEXCOORD2",	"vofi_TexCoord2",		AT_PS_IN },
	{ "half4",		"htexcoord3",	"TEXCOORD3",	"vofi_TexCoord3",		AT_PS_IN },
	{ "half4",		"htexcoord4",	"TEXCOORD4",	"vofi_TexCoord4",		AT_PS_IN },
	{ "half4",		"htexcoord5",	"TEXCOORD5",	"vofi_TexCoord5",		AT_PS_IN },
	{ "half4",		"htexcoord6",	"TEXCOORD6",	"vofi_TexCoord6",		AT_PS_IN },
	{ "half4",		"htexcoord7",	"TEXCOORD7",	"vofi_TexCoord7",		AT_PS_IN },
	{ "half4",		"htexcoord8",	"TEXCOORD8",	"vofi_TexCoord8",		AT_PS_IN },
	{ "half4",		"htexcoord9",	"TEXCOORD9",	"vofi_TexCoord9",		AT_PS_IN },
	{ "float",		"fog",			"FOG",			"gl_FogFragCoord",		AT_VS_OUT },
	{ "float4",		"fog",			"FOG",			"gl_FogFragCoord",		AT_PS_IN },
};

struct typeConversion_t {
	const char * typeCG;
	const char * typeGLSL;
} typeConversion[] = {
	{ "void",				"void" },

	{ "fixed",				"float" },

	{ "float",				"float" },
	{ "float2",				"vec2" },
	{ "float3",				"vec3" },
	{ "float4",				"vec4" },

	{ "half",				"float" },
	{ "half2",				"vec2" },
	{ "half3",				"vec3" },
	{ "half4",				"vec4" },

	{ "int",				"int" },
	{ "int2",				"ivec2" },
	{ "int3",				"ivec3" },
	{ "int4",				"ivec4" },

	{ "bool",				"bool" },
	{ "bool2",				"bvec2" },
	{ "bool3",				"bvec3" },
	{ "bool4",				"bvec4" },

	{ "float2x2",			"mat2x2" },
	{ "float2x3",			"mat2x3" },
	{ "float2x4",			"mat2x4" },

	{ "float3x2",			"mat3x2" },
	{ "float3x3",			"mat3x3" },
	{ "float3x4",			"mat3x4" },

	{ "float4x2",			"mat4x2" },
	{ "float4x3",			"mat4x3" },
	{ "float4x4",			"mat4x4" },

	{ "sampler1D",			"sampler1D" },
	{ "sampler2D",			"sampler2D" },
	{ "sampler3D",			"sampler3D" },
	{ "samplerCUBE",		"samplerCube" },

	{ "sampler1DShadow",	"sampler1DShadow" },
	{ "sampler2DShadow",	"sampler2DShadow" },
	{ "sampler3DShadow",	"sampler3DShadow" },
	{ "samplerCubeShadow",	"samplerCubeShadow" },

	{ "sampler2DMS",		"sampler2DMS" },

	{ NULL, NULL }
};

const char * vertexInsert = {
	"#version 150\n"
	"#define PC\n"
	"\n"
	"float saturate( float v ) { return clamp( v, 0.0, 1.0 ); }\n"
	"vec2 saturate( vec2 v ) { return clamp( v, 0.0, 1.0 ); }\n"
	"vec3 saturate( vec3 v ) { return clamp( v, 0.0, 1.0 ); }\n"
	"vec4 saturate( vec4 v ) { return clamp( v, 0.0, 1.0 ); }\n"
	"vec4 tex2Dlod( sampler2D sampler, vec4 texcoord ) { return textureLod( sampler, texcoord.xy, texcoord.w ); }\n"
	"\n"
};

const char * fragmentInsert = {
	"#version 150\n"
	"#define PC\n"
	"\n"
	"void clip( float v ) { if ( v < 0.0 ) { discard; } }\n"
	"void clip( vec2 v ) { if ( any( lessThan( v, vec2( 0.0 ) ) ) ) { discard; } }\n"
	"void clip( vec3 v ) { if ( any( lessThan( v, vec3( 0.0 ) ) ) ) { discard; } }\n"
	"void clip( vec4 v ) { if ( any( lessThan( v, vec4( 0.0 ) ) ) ) { discard; } }\n"
	"\n"
	"float saturate( float v ) { return clamp( v, 0.0, 1.0 ); }\n"
	"vec2 saturate( vec2 v ) { return clamp( v, 0.0, 1.0 ); }\n"
	"vec3 saturate( vec3 v ) { return clamp( v, 0.0, 1.0 ); }\n"
	"vec4 saturate( vec4 v ) { return clamp( v, 0.0, 1.0 ); }\n"
	"\n"
	"vec4 tex2D( sampler2D sampler, vec2 texcoord ) { return texture( sampler, texcoord.xy ); }\n"
	"vec4 tex2D( sampler2DShadow sampler, vec3 texcoord ) { return vec4( texture( sampler, texcoord.xyz ) ); }\n"
	"\n"
	"vec4 tex2D( sampler2D sampler, vec2 texcoord, vec2 dx, vec2 dy ) { return textureGrad( sampler, texcoord.xy, dx, dy ); }\n"
	"vec4 tex2D( sampler2DShadow sampler, vec3 texcoord, vec2 dx, vec2 dy ) { return vec4( textureGrad( sampler, texcoord.xyz, dx, dy ) ); }\n"
	"\n"
	"vec4 texCUBE( samplerCube sampler, vec3 texcoord ) { return texture( sampler, texcoord.xyz ); }\n"
	"vec4 texCUBE( samplerCubeShadow sampler, vec4 texcoord ) { return vec4( texture( sampler, texcoord.xyzw ) ); }\n"
	"\n"
	"vec4 tex1Dproj( sampler1D sampler, vec2 texcoord ) { return textureProj( sampler, texcoord ); }\n"
	"vec4 tex2Dproj( sampler2D sampler, vec3 texcoord ) { return textureProj( sampler, texcoord ); }\n"
	"vec4 tex3Dproj( sampler3D sampler, vec4 texcoord ) { return textureProj( sampler, texcoord ); }\n"
	"\n"
	"vec4 tex1Dbias( sampler1D sampler, vec4 texcoord ) { return texture( sampler, texcoord.x, texcoord.w ); }\n"
	"vec4 tex2Dbias( sampler2D sampler, vec4 texcoord ) { return texture( sampler, texcoord.xy, texcoord.w ); }\n"
	"vec4 tex3Dbias( sampler3D sampler, vec4 texcoord ) { return texture( sampler, texcoord.xyz, texcoord.w ); }\n"
	"vec4 texCUBEbias( samplerCube sampler, vec4 texcoord ) { return texture( sampler, texcoord.xyz, texcoord.w ); }\n"
	"\n"
	"vec4 tex1Dlod( sampler1D sampler, vec4 texcoord ) { return textureLod( sampler, texcoord.x, texcoord.w ); }\n"
	"vec4 tex2Dlod( sampler2D sampler, vec4 texcoord ) { return textureLod( sampler, texcoord.xy, texcoord.w ); }\n"
	"vec4 tex3Dlod( sampler3D sampler, vec4 texcoord ) { return textureLod( sampler, texcoord.xyz, texcoord.w ); }\n"
	"vec4 texCUBElod( samplerCube sampler, vec4 texcoord ) { return textureLod( sampler, texcoord.xyz, texcoord.w ); }\n"
	"\n"
};

struct builtinConversion_t {
	const char * nameCG;
	const char * nameGLSL;
} builtinConversion[] = {
	{ "frac",		"fract" },
	{ "lerp",		"mix" },
	{ "rsqrt",		"inversesqrt" },
	{ "ddx",		"dFdx" },
	{ "ddy",		"dFdy" },

	{ NULL, NULL }
};

struct inOutVariable_t {
	idStr	type;
	idStr	nameCg;
	idStr	nameGLSL;
	bool	declareInOut;
};

/*
========================
ParseInOutStruct
========================
*/
void ParseInOutStruct( idLexer & src, int attribType, idList< inOutVariable_t > & inOutVars ) {
	src.ExpectTokenString( "{" );

	while( !src.CheckTokenString( "}" ) ) {
		inOutVariable_t var;

		idToken token;
		src.ReadToken( &token );
		var.type = token;
		src.ReadToken( &token );
		var.nameCg = token;

		if ( !src.CheckTokenString( ":" ) ) {
			src.SkipUntilString( ";" );
			continue;
		}

		src.ReadToken( &token );
		var.nameGLSL = token;
		src.ExpectTokenString( ";" );

		// convert the type
		for ( int i = 0; typeConversion[i].typeCG != NULL; i++ ) {
			if ( var.type.Cmp( typeConversion[i].typeCG ) == 0 ) {
				var.type = typeConversion[i].typeGLSL;
				break;
			}
		}

		// convert the semantic to a GLSL name
		if ( attribType == AT_VS_IN ) {
			const char * glsl = GL_VertexInputName( R_VertexAttribIndex( var.nameGLSL.c_str() ) );
			if ( glsl != nullptr ) {
				var.nameGLSL = glsl;
			}
		} else {
			for ( const attribInfo_t & attrib : attribsPC ) {
				if ( ( attrib.flags & attribType ) != 0 && var.nameGLSL.Cmp( attrib.semantic ) == 0 ) {
					var.nameGLSL = attrib.glsl;
					break;
				}
			}
		}

		// check if it was defined previously
		var.declareInOut = true;
		for ( int i = 0; i < inOutVars.Num(); i++ ) {
			if ( var.nameGLSL == inOutVars[i].nameGLSL ) {
				var.declareInOut = false;
				break;
			}
		}

		inOutVars.Append( var );
	}

	src.ExpectTokenString( ";" );
}

/*
========================
ConvertCG2GLSL
========================
*/
idStr ConvertCG2GLSL( const idStr & in, const char * name, bool isVertexProgram, idStr & uniforms ) {
	idStr program;
	program.ReAllocate( in.Length() * 2, false );

	idList< inOutVariable_t, TAG_RENDERPROG > varsIn;
	idList< inOutVariable_t, TAG_RENDERPROG > varsOut;
	idList< idStr > uniformList;

	idLexer src( LEXFL_NOFATALERRORS );
	src.LoadMemory( in.c_str(), in.Length(), name );

	bool inMain = false;
	const char * uniformArrayName = isVertexProgram ? VERTEX_UNIFORM_ARRAY_NAME : FRAGMENT_UNIFORM_ARRAY_NAME;
	char newline[128] = { "\n" };

	idToken token;
	while ( src.ReadToken( &token ) ) {

		// check for uniforms
		while ( token == "uniform" && src.CheckTokenString( "float4" ) ) {
			src.ReadToken( &token );
			uniformList.Append( token );

			// strip ': register()' from uniforms
			if ( src.CheckTokenString( ":" ) ) {
				if ( src.CheckTokenString( "register" ) ) {
					src.SkipUntilString( ";" );
				}
			}

			src.ReadToken( & token );
		}

		// convert the in/out structs
		if ( token == "struct" ) {
			if ( src.CheckTokenString( "VS_IN" ) ) {
				ParseInOutStruct( src, AT_VS_IN, varsIn );
				program += "\n\n";
				for ( int i = 0; i < varsIn.Num(); i++ ) {
					if ( varsIn[i].declareInOut ) {
						program += "in " + varsIn[i].type + " " + varsIn[i].nameGLSL + ";\n";
					}
				}
				continue;
			} else if ( src.CheckTokenString( "VS_OUT" ) ) {
				ParseInOutStruct( src, AT_VS_OUT, varsOut );
				program += "\n";
				for ( int i = 0; i < varsOut.Num(); i++ ) {
					if ( varsOut[i].declareInOut ) {
						program += "out " + varsOut[i].type + " " + varsOut[i].nameGLSL + ";\n";
					}
				}
				continue;
			} else if ( src.CheckTokenString( "PS_IN" ) ) {
				ParseInOutStruct( src, AT_PS_IN, varsIn );
				program += "\n\n";
				for ( int i = 0; i < varsIn.Num(); i++ ) {
					if ( varsIn[i].declareInOut ) {
						program += "in " + varsIn[i].type + " " + varsIn[i].nameGLSL + ";\n";
					}
				}
				inOutVariable_t var;
				var.type = "vec4";
				var.nameCg = "position";
				var.nameGLSL = "gl_FragCoord";
				varsIn.Append( var );
				continue;
			} else if ( src.CheckTokenString( "PS_OUT" ) ) {
				ParseInOutStruct( src, AT_PS_OUT, varsOut );
				program += "\n";
				for ( int i = 0; i < varsOut.Num(); i++ ) {
					if ( varsOut[i].declareInOut ) {
						program += "out " + varsOut[i].type + " " + varsOut[i].nameGLSL + ";\n";
					}
				}
				continue;
			}
		}

		// strip 'static'
		if ( token == "static" ) {
			program += ( token.linesCrossed > 0 ) ? newline : ( token.WhiteSpaceBeforeToken() > 0 ? " " : "" );
			src.SkipWhiteSpace( true ); // remove white space between 'static' and the next token
			continue;
		}

		// strip ': register()' from uniforms
		if ( token == ":" ) {
			if ( src.CheckTokenString( "register" ) ) {
				src.SkipUntilString( ";" );
				program += ";";
				continue;
			}
		}

		// strip in/program parameters from main
		if ( token == "void" && src.CheckTokenString( "main" ) ) {
			src.ExpectTokenString( "(" );
			while( src.ReadToken( &token ) ) {
				if ( token == ")" ) {
					break;
				}
			}

			program += "\nvoid main()";
			inMain = true;
			continue;
		}

		// strip 'const' from local variables in main()
		if ( token == "const" && inMain ) {
			program += ( token.linesCrossed > 0 ) ? newline : ( token.WhiteSpaceBeforeToken() > 0 ? " " : "" );
			src.SkipWhiteSpace( true ); // remove white space between 'const' and the next token
			continue;
		}

		// maintain indentation
		if ( token == "{" ) {
			program += ( token.linesCrossed > 0 ) ? newline : ( token.WhiteSpaceBeforeToken() > 0 ? " " : "" );
			program += "{";

			int len = Min( idStr::Length( newline ) + 1, (int)sizeof( newline ) - 1 );
			newline[len - 1] = '\t';
			newline[len - 0] = '\0';
			continue;
		}
		if ( token == "}" ) {
			int len = Max( idStr::Length( newline ) - 1, 0 );
			newline[len] = '\0';

			program += ( token.linesCrossed > 0 ) ? newline : ( token.WhiteSpaceBeforeToken() > 0 ? " " : "" );
			program += "}";
			continue;
		}

		// check for a type conversion
		bool foundType = false;
		for ( int i = 0; typeConversion[i].typeCG != NULL; i++ ) {
			if ( token.Cmp( typeConversion[i].typeCG ) == 0 ) {
				program += ( token.linesCrossed > 0 ) ? newline : ( token.WhiteSpaceBeforeToken() > 0 ? " " : "" );
				program += typeConversion[i].typeGLSL;
				foundType = true;
				break;
			}
		}
		if ( foundType ) {
			continue;
		}

		// check for uniforms that need to be converted to the array
		bool isUniform = false;
		for ( int i = 0; i < uniformList.Num(); i++ ) {
			if ( token == uniformList[i] ) {
				program += ( token.linesCrossed > 0 ) ? newline : ( token.WhiteSpaceBeforeToken() > 0 ? " " : "" );
				program += va( "%s[%d /* %s */]", uniformArrayName, i, uniformList[i].c_str() );
				isUniform = true;
				break;
			}
		}
		if ( isUniform ) {
			continue;
		}

		// check for input/output parameters
		if ( src.CheckTokenString( "." ) ) {

			if ( token == "vertex" || token == "fragment" ) {
				idToken member;
				src.ReadToken( &member );

				bool foundInOut = false;
				for ( int i = 0; i < varsIn.Num(); i++ ) {
					if ( member.Cmp( varsIn[i].nameCg ) == 0 ) {
						program += ( token.linesCrossed > 0 ) ? newline : ( token.WhiteSpaceBeforeToken() > 0 ? " " : "" );
						program += varsIn[i].nameGLSL;
						foundInOut = true;
						break;
					}
				}
				if ( !foundInOut ) {
					src.Error( "invalid input parameter %s.%s", token.c_str(), member.c_str() );
					program += ( token.linesCrossed > 0 ) ? newline : ( token.WhiteSpaceBeforeToken() > 0 ? " " : "" );
					program += token;
					program += ".";
					program += member;
				}
				continue;
			}

			if ( token == "result" ) {
				idToken member;
				src.ReadToken( &member );

				bool foundInOut = false;
				for ( int i = 0; i < varsOut.Num(); i++ ) {
					if ( member.Cmp( varsOut[i].nameCg ) == 0 ) {
						program += ( token.linesCrossed > 0 ) ? newline : ( token.WhiteSpaceBeforeToken() > 0 ? " " : "" );
						program += varsOut[i].nameGLSL;
						foundInOut = true;
						break;
					}
				}
				if ( !foundInOut ) {
					src.Error( "invalid output parameter %s.%s", token.c_str(), member.c_str() );
					program += ( token.linesCrossed > 0 ) ? newline : ( token.WhiteSpaceBeforeToken() > 0 ? " " : "" );
					program += token;
					program += ".";
					program += member;
				}
				continue;
			}

			program += ( token.linesCrossed > 0 ) ? newline : ( token.WhiteSpaceBeforeToken() > 0 ? " " : "" );
			program += token;
			program += ".";
			continue;
		}

		// check for a function conversion
		bool foundFunction = false;
		for ( int i = 0; builtinConversion[i].nameCG != NULL; i++ ) {
			if ( token.Cmp( builtinConversion[i].nameCG ) == 0 ) {
				program += ( token.linesCrossed > 0 ) ? newline : ( token.WhiteSpaceBeforeToken() > 0 ? " " : "" );
				program += builtinConversion[i].nameGLSL;
				foundFunction = true;
				break;
			}
		}
		if ( foundFunction ) {
			continue;
		}

		program += ( token.linesCrossed > 0 ) ? newline : ( token.WhiteSpaceBeforeToken() > 0 ? " " : "" );
		program += token;
	}

	idStr out;

	if ( isVertexProgram ) {
		out.ReAllocate( idStr::Length( vertexInsert ) + in.Length() * 2, false );
		out += vertexInsert;
	} else {
		out.ReAllocate( idStr::Length( fragmentInsert ) + in.Length() * 2, false );
		out += fragmentInsert;
	}

	if ( uniformList.Num() > 0 ) {
		out += va( "\nuniform vec4 %s[%d];\n", uniformArrayName, uniformList.Num() );
	}

	out += program;

	for ( int i = 0; i < uniformList.Num(); i++ ) {
		uniforms += uniformList[i];
		uniforms += "\n";
	}
	uniforms += "\n";

	return out;
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
================================================================================================
idRenderBackendGL::CreateShaderStage

Loads renderprogs/glsl/<name>_vertex.glsl or _fragment.glsl with its .uniforms list, converted from
renderprogs/<name>.vertex or .pixel when those are newer. A stage without a source file is a record
without a shader, not NONE: shared code never looks for it again and its program fails to link.
================================================================================================
*/
shaderHandle_t idRenderBackendGL::CreateShaderStage( shaderStageType_t stage, const char * name, shaderStageInfo_t & info ) {
	const GLenum target = ( stage == shaderStageType_t::FRAGMENT ) ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER;
	info.numParms = 0;

	const idStr inFile = R_RenderProgPath( name, stage );
	idStr outFileHLSL;
	idStr outFileGLSL;
	idStr outFileUniforms;
	outFileHLSL.Format( "renderprogs\\glsl\\%s", name );
	outFileHLSL.StripFileExtension();
	outFileGLSL.Format( "renderprogs\\glsl\\%s", name );
	outFileGLSL.StripFileExtension();
	outFileUniforms.Format( "renderprogs\\glsl\\%s", name );
	outFileUniforms.StripFileExtension();
	if ( target == GL_FRAGMENT_SHADER ) {
		outFileHLSL += "_fragment.hlsl";
		outFileGLSL += "_fragment.glsl";
		outFileUniforms += "_fragment.uniforms";
	} else {
		outFileHLSL += "_vertex.hlsl";
		outFileGLSL += "_vertex.glsl";
		outFileUniforms += "_vertex.uniforms";
	}

	// first check whether we already have a valid GLSL file and compare it to the hlsl timestamp;
	ID_TIME_T hlslTimeStamp;
	int hlslFileLength = fileSystem->ReadFile( inFile.c_str(), NULL, &hlslTimeStamp );

	ID_TIME_T glslTimeStamp;
	int glslFileLength = fileSystem->ReadFile( outFileGLSL.c_str(), NULL, &glslTimeStamp );

	// if the glsl file doesn't exist or we have a newer HLSL file we need to recreate the glsl file.
	idStr programGLSL;
	idStr programUniforms;
	if ( ( glslFileLength <= 0 ) || ( hlslTimeStamp > glslTimeStamp ) ) {
		if ( hlslFileLength <= 0 ) {
			// hlsl file doesn't even exist bail out
			return GL_NewShaderStage( 0, name, info );
		}

		// nullopt after the file was found: unreadable, or a uniform no parm names (warned)
		const std::optional<renderProgSource_t> source = R_LoadRenderProgSource( name, stage );
		if ( !source ) {
			return GL_NewShaderStage( 0, name, info );
		}
		const idStr & programHLSL = source->text;
		programGLSL = ConvertCG2GLSL( programHLSL, inFile, target == GL_VERTEX_SHADER, programUniforms );

		fileSystem->WriteFile( outFileHLSL, programHLSL.c_str(), programHLSL.Length(), "fs_basepath" );
		fileSystem->WriteFile( outFileGLSL, programGLSL.c_str(), programGLSL.Length(), "fs_basepath" );
		fileSystem->WriteFile( outFileUniforms, programUniforms.c_str(), programUniforms.Length(), "fs_basepath" );
	} else {
		// read in the glsl file
		void * fileBufferGLSL = NULL;
		int lengthGLSL = fileSystem->ReadFile( outFileGLSL.c_str(), &fileBufferGLSL );
		if ( lengthGLSL <= 0 ) {
			idLib::Error( "GLSL file %s could not be loaded and may be corrupt", outFileGLSL.c_str() );
		}
		programGLSL = ( const char * ) fileBufferGLSL;
		Mem_Free( fileBufferGLSL );

		// read in the uniform file
		void * fileBufferUniforms = NULL;
		int lengthUniforms = fileSystem->ReadFile( outFileUniforms.c_str(), &fileBufferUniforms );
		if ( lengthUniforms <= 0 ) {
			idLib::Error( "uniform file %s could not be loaded and may be corrupt", outFileUniforms.c_str() );
		}
		programUniforms = ( const char* ) fileBufferUniforms;
		Mem_Free( fileBufferUniforms );
	}

	// find the uniforms locations in either the vertex or fragment uniform array
	idLexer src( programUniforms, programUniforms.Length(), "uniforms" );
	idToken token;
	while ( src.ReadToken( &token ) ) {
		const std::optional<renderParm_t> parm = R_FindRenderParm( token.c_str() );
		if ( !parm ) {
			idLib::Error( "couldn't find uniform %s for %s", token.c_str(), outFileGLSL.c_str() );
		}
		if ( info.numParms == MAX_RENDER_PARMS ) {
			idLib::Error( "more than %d uniforms for %s", MAX_RENDER_PARMS, outFileGLSL.c_str() );
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
				idLib::Printf( "While compiling %s program %s\n", ( target == GL_FRAGMENT_SHADER ) ? "fragment" : "vertex" , inFile.c_str() );

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

