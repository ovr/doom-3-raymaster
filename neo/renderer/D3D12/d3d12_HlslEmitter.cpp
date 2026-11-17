/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
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
#include "../../idlib/precompiled.h"

#include "d3d12_HlslEmitter.h"

#include <algorithm>
#include <vector>

namespace {

// Overloads on the texture and sampler pair a Cg sampler becomes; tex2Dproj( float4 ) divides by w as Cg does.
constexpr const char * PRELUDE =
	"float4 idGLClipToD3D( float4 p ) { return float4( p.x, -p.y, ( p.z + p.w ) * 0.5, p.w ); }\n"
	"float4 tex2D( Texture2D t, SamplerState s, float2 uv ) { return t.Sample( s, uv ); }\n"
	"float4 tex2Dproj( Texture2D t, SamplerState s, float3 uvw ) { return t.Sample( s, uvw.xy / uvw.z ); }\n"
	"float4 tex2Dproj( Texture2D t, SamplerState s, float4 uvw ) { return t.Sample( s, uvw.xy / uvw.w ); }\n"
	"float4 tex2Dlod( Texture2D t, SamplerState s, float4 uv ) { return t.SampleLevel( s, uv.xy, uv.w ); }\n"
	"float4 tex2Dbias( Texture2D t, SamplerState s, float4 uv ) { return t.SampleBias( s, uv.xy, uv.w ); }\n"
	"float4 texCUBE( TextureCube t, SamplerState s, float3 dir ) { return t.Sample( s, dir ); }\n";

// E15: GLSL leaves pow( x < 0, y ) undefined. FXC's exp2( y * log2( x ) ) is NaN, a black pixel, while AMD's GL
// driver unrolls the interactions' pow( hDotN, 10 ) into products. Added when the source calls pow; per component.
constexpr const char * POW_PRELUDE =
	"float idPow( float x, float y ) { return pow( ( frac( y * 0.5 ) == 0.0 ) ? abs( x ) : x, y ); }\n"
	"float2 idPow( float2 x, float2 y ) { return pow( ( frac( y * 0.5 ) == 0.0 ) ? abs( x ) : x, y ); }\n"
	"float3 idPow( float3 x, float3 y ) { return pow( ( frac( y * 0.5 ) == 0.0 ) ? abs( x ) : x, y ); }\n"
	"float4 idPow( float4 x, float4 y ) { return pow( ( frac( y * 0.5 ) == 0.0 ) ? abs( x ) : x, y ); }\n";

struct token_t {
	idStr				space;		// the whitespace before the token
	idStr				text;		// as in the source
	int					type = 0;	// TT_*
};
using tokens_t = std::vector<token_t>;

// A top-level declaration: the tokens [begin, end).
enum class declKind_t { PARM, JOINTS, SAMPLER, ENTRY_STRUCT, FUNCTION, MAIN, OTHER };
struct decl_t {
	declKind_t			kind = declKind_t::OTHER;
	int					begin = 0;
	int					end = 0;
	int					open = -1;	// FUNCTION, MAIN: the '(' of the parameters
};

struct entryMember_t {
	idStr				type;		// half, vec mapped to float
	idStr				name;
	idStr				semantic;	// upper case, without _CENTROID
	idStr				written;	// the semantic as written
};

struct entryStruct_t {
	bool				declared = false;
	std::vector<entryMember_t>	members;
};

enum class entry_t { VS_IN, VS_OUT, PS_IN, PS_OUT, COUNT };
constexpr const char * ENTRY_NAMES[] = { "VS_IN", "VS_OUT", "PS_IN", "PS_OUT" };
static_assert( std::size( ENTRY_NAMES ) == static_cast<size_t>( entry_t::COUNT ) );

using unexpected_t = std::unexpected<idStr>;

[[nodiscard]] unexpected_t Fail( const char * name, const char * what ) {
	return unexpected_t( idStr( va( "%s: %s", name, what ) ) );
}

// Without the CRs of the source, so the HLSL and its cache key do not depend on the checkout's line ends.
[[nodiscard]] tokens_t Tokenize( const idStr & source, const char * name ) {
	idStr text = source;
	text.Replace( "\r", "" );
	idLexer lexer( LEXFL_NOERRORS | LEXFL_NOWARNINGS | LEXFL_NOFATALERRORS | LEXFL_NOSTRINGCONCAT );
	lexer.LoadMemory( text.c_str(), text.Length(), name );
	tokens_t tokens;
	idToken token;
	int end = 0;
	while ( lexer.ReadToken( &token ) ) {
		const int start = lexer.GetLastWhiteSpaceEnd();
		const int stop = lexer.GetFileOffset();
		token_t & t = tokens.emplace_back();
		t.space.Append( text.c_str() + end, start - end );
		t.text.Append( text.c_str() + start, stop - start );
		t.type = token.type;
		end = stop;
	}
	return tokens;
}

[[nodiscard]] bool IsOpen( const idStr & t ) { return t == "(" || t == "[" || t == "{"; }
[[nodiscard]] bool IsClose( const idStr & t ) { return t == ")" || t == "]" || t == "}"; }

// The index of the bracket that closes tokens[open], or -1.
[[nodiscard]] int Match( const tokens_t & tokens, int open ) {
	int depth = 0;
	for ( int i = open; i < static_cast<int>( tokens.size() ); i++ ) {
		if ( IsOpen( tokens[i].text ) ) {
			depth++;
		} else if ( IsClose( tokens[i].text ) && --depth == 0 ) {
			return i;
		}
	}
	return -1;
}

// The '(' at tokens[open] holds exactly one argument.
[[nodiscard]] bool SingleArgument( const tokens_t & tokens, int open ) {
	const int close = Match( tokens, open );
	if ( close < 0 || close == open + 1 ) {
		return false;
	}
	int depth = 0;
	for ( int i = open + 1; i < close; i++ ) {
		const idStr & t = tokens[i].text;
		if ( IsOpen( t ) ) {
			depth++;
		} else if ( IsClose( t ) ) {
			depth--;
		} else if ( depth == 0 && t == "," ) {
			return false;
		}
	}
	return true;
}

// E5, E14
[[nodiscard]] idStr MapName( const idStr & name ) {
	if ( name.Cmpn( "half", 4 ) == 0 && ( name.Length() == 4 || idStr::CharIsNumeric( name[4] ) ) ) {
		return idStr( "float" ) + ( name.c_str() + 4 );
	}
	if ( name == "vec2" || name == "vec3" || name == "vec4" ) {
		return idStr( "float" ) + ( name.c_str() + 3 );
	}
	if ( name == "fract" ) {
		return "frac";
	}
	return name;
}

// float, float2 .. float4 (after MapName) → 1 .. 4; 0 for anything else.
[[nodiscard]] int Components( const idStr & type ) {
	for ( const char * scalar : { "float", "int", "uint", "bool" } ) {
		const int length = idStr::Length( scalar );
		if ( type.Cmpn( scalar, length ) != 0 ) {
			continue;
		}
		if ( type.Length() == length ) {
			return 1;
		}
		if ( type.Length() == length + 1 && type[length] >= '2' && type[length] <= '4' ) {
			return type[length] - '0';
		}
	}
	return 0;
}

[[nodiscard]] bool IsVectorType( const idStr & type ) {
	return Components( type ) > 1;
}

[[nodiscard]] const char * FloatType( int components ) {
	constexpr const char * TYPES[] = { "float", "float", "float2", "float3", "float4" };
	return TYPES[idMath::ClampInt( 0, 4, components )];
}

// TEXCOORDn → n, COLOR / COLOR0 → 10, COLOR1 → 11, else -1.
[[nodiscard]] int VaryingBit( const idStr & semantic ) {
	if ( semantic.Cmpn( "TEXCOORD", 8 ) == 0 && semantic.Length() == 9 && idStr::CharIsNumeric( semantic[8] ) ) {
		return semantic[8] - '0';
	}
	if ( semantic == "COLOR" || semantic == "COLOR0" ) {
		return D3D12_VARYING_COLOR0;
	}
	if ( semantic == "COLOR1" ) {
		return D3D12_VARYING_COLOR1;
	}
	return -1;
}

[[nodiscard]] idStr VaryingName( int bit ) {
	return ( bit >= D3D12_VARYING_COLOR0 ) ? va( "c%d", bit - D3D12_VARYING_COLOR0 ) : va( "t%d", bit );
}

[[nodiscard]] idStr VaryingSemantic( int bit ) {
	return ( bit >= D3D12_VARYING_COLOR0 ) ? va( "COLOR%d", bit - D3D12_VARYING_COLOR0 ) : va( "TEXCOORD%d", bit );
}

// expr of n components as a float4, the rest 0.
[[nodiscard]] idStr Widen( const idStr & expr, int components ) {
	constexpr const char * PADDING[] = { "", ", 0.0, 0.0, 0.0", ", 0.0, 0.0", ", 0.0", "" };
	if ( components >= 4 ) {
		return expr;
	}
	return idStr( "float4( " ) + expr + PADDING[components] + " )";
}

[[nodiscard]] const char * Narrow( int components ) {
	constexpr const char * SWIZZLES[] = { "", ".x", ".xy", ".xyz", "" };
	return SWIZZLES[idMath::ClampInt( 0, 4, components )];
}

// Splits the text into declarations: each ends at a ';' outside brackets, a function after its body.
[[nodiscard]] std::expected<std::vector<decl_t>, idStr> SplitDeclarations( const tokens_t & tokens, const char * name ) {
	std::vector<decl_t> decls;
	const int count = static_cast<int>( tokens.size() );
	int i = 0;
	while ( i < count ) {
		decl_t decl;
		decl.begin = i;
		for ( ;; ) {
			if ( i >= count ) {
				return Fail( name, va( "the declaration at '%s' has no end", tokens[decl.begin].text.c_str() ) );
			}
			const idStr & t = tokens[i].text;
			if ( t == ";" ) {
				i++;
				break;
			}
			if ( IsClose( t ) ) {
				return Fail( name, va( "unbalanced '%s'", t.c_str() ) );
			}
			if ( !IsOpen( t ) ) {
				i++;
				continue;
			}
			const int close = Match( tokens, i );
			if ( close < 0 ) {
				return Fail( name, va( "unbalanced '%s'", t.c_str() ) );
			}
			if ( t == "(" && decl.open < 0 ) {
				decl.open = i;
			}
			const bool body = ( t == "{" && i > 0 && tokens[i - 1].text == ")" );
			i = close + 1;
			if ( body ) {
				break;
			}
		}
		decl.end = i;
		decls.push_back( decl );
	}
	return decls;
}

[[nodiscard]] std::optional<entry_t> EntryStruct( const tokens_t & tokens, const decl_t & decl ) {
	if ( decl.end - decl.begin < 3 || tokens[decl.begin].text != "struct" ) {
		return std::nullopt;
	}
	for ( int i = 0; i < static_cast<int>( entry_t::COUNT ); i++ ) {
		if ( tokens[decl.begin + 1].text == ENTRY_NAMES[i] ) {
			return static_cast<entry_t>( i );
		}
	}
	return std::nullopt;
}

// struct NAME { type name [: semantic]; ... };
[[nodiscard]] std::expected<std::vector<entryMember_t>, idStr> ParseMembers( const tokens_t & tokens, const decl_t & decl, const char * name ) {
	std::vector<entryMember_t> members;
	const char * structName = tokens[decl.begin + 1].text.c_str();
	int i = decl.begin + 2;
	if ( tokens[i].text != "{" ) {
		return Fail( name, va( "struct %s has no body", structName ) );
	}
	const int close = Match( tokens, i );
	for ( i++; i < close; ) {
		if ( i + 2 >= close || tokens[i].type != TT_NAME || tokens[i + 1].type != TT_NAME ) {
			return Fail( name, va( "struct %s: '%s' is no member", structName, tokens[i].text.c_str() ) );
		}
		entryMember_t & member = members.emplace_back();
		member.type = MapName( tokens[i].text );
		member.name = tokens[i + 1].text;
		i += 2;
		if ( tokens[i].text == ":" && i + 1 < close && tokens[i + 1].type == TT_NAME ) {
			member.written = tokens[i + 1].text;
			member.semantic = member.written;
			member.semantic.ToUpper();
			if ( member.semantic.Right( 9 ) == "_CENTROID" ) {
				member.semantic.CapLength( member.semantic.Length() - 9 );
			}
			i += 2;
		}
		if ( tokens[i].text != ";" ) {
			return Fail( name, va( "struct %s: '%s' after member %s", structName, tokens[i].text.c_str(), member.name.c_str() ) );
		}
		i++;
	}
	return members;
}

struct mainSignature_t {
	int					input = -1;			// the entry_t of the input parameter, -1 without one
	idStr				inputName;
	int					output = -1;
};

// main( [VS_IN|PS_IN name,] out VS_OUT|PS_OUT name )
[[nodiscard]] std::expected<mainSignature_t, idStr> ParseMain( const tokens_t & tokens, const decl_t & decl, const char * name ) {
	mainSignature_t signature;
	const int close = Match( tokens, decl.open );
	int i = decl.open + 1;
	while ( i < close ) {
		bool out = false;
		while ( tokens[i].text == "in" || tokens[i].text == "out" || tokens[i].text == "inout" ) {
			out = out || ( tokens[i].text != "in" );
			i++;
		}
		if ( i + 1 >= close || tokens[i].type != TT_NAME || tokens[i + 1].type != TT_NAME ) {
			return Fail( name, va( "main: '%s' is no parameter", tokens[i].text.c_str() ) );
		}
		int type = -1;
		for ( int e = 0; e < static_cast<int>( entry_t::COUNT ); e++ ) {
			if ( tokens[i].text == ENTRY_NAMES[e] ) {
				type = e;
			}
		}
		const bool isInput = ( type == static_cast<int>( entry_t::VS_IN ) || type == static_cast<int>( entry_t::PS_IN ) );
		if ( type < 0 || isInput == out ) {
			return Fail( name, va( "main: the parameter %s %s is neither an input struct nor an out struct", tokens[i].text.c_str(), tokens[i + 1].text.c_str() ) );
		}
		if ( isInput ) {
			signature.input = type;
			signature.inputName = tokens[i + 1].text;
		} else {
			signature.output = type;
		}
		i += 2;
		if ( i < close && tokens[i].text == "," ) {
			i++;
		}
	}
	return signature;
}

struct scope_t {
	const std::vector<idStr> *	globalSamplers = nullptr;
	std::vector<idStr>			parameterSamplers;
	bool						contains( const idStr & name ) const {
		for ( const std::vector<idStr> * list : { globalSamplers, &parameterSamplers } ) {
			if ( list != nullptr && std::ranges::find( *list, name ) != list->end() ) {
				return true;
			}
		}
		return false;
	}
};

// The tokens [begin, end) into out, E4-E6, E14, E15 and for main E9; the first token's whitespace is the caller's.
void Rewrite( const tokens_t & tokens, int begin, int end, const scope_t & scope, const decl_t * main, idStr & out ) {
	const int mainClose = ( main != nullptr ) ? Match( tokens, main->open ) : -1;
	std::vector<bool> casts;	// per open '(': whether it closes a cast with "))"
	for ( int i = begin; i < end; i++ ) {
		const token_t & t = tokens[i];
		if ( i > begin ) {
			out += t.space;
		}
		if ( t.text == "(" ) {
			casts.push_back( false );
			out += "(";
			continue;
		}
		if ( t.text == ")" ) {
			const bool cast = !casts.empty() && casts.back();
			if ( !casts.empty() ) {
				casts.pop_back();
			}
			out += cast ? "))" : ")";
			continue;
		}
		if ( t.type != TT_NAME || ( i > 0 && tokens[i - 1].text == "." ) ) {
			out += t.text;
			continue;
		}
		const idStr mapped = MapName( t.text );
		const bool next = ( i + 1 < end );
		if ( next && IsVectorType( mapped ) && tokens[i + 1].text == "(" && SingleArgument( tokens, i + 1 ) ) {
			out += "((" + mapped + ")(";
			i++;
			casts.push_back( true );
		} else if ( next && ( mapped == "sampler2D" || mapped == "samplerCUBE" ) && tokens[i + 1].type == TT_NAME ) {
			const idStr & parameter = tokens[++i].text;
			out += ( mapped == "sampler2D" ) ? "Texture2D " : "TextureCube ";
			out += parameter + "_t, SamplerState " + parameter + "_s";
		} else if ( scope.contains( t.text ) ) {
			out += t.text + "_t, " + t.text + "_s";
		} else if ( next && mapped == "pow" && tokens[i + 1].text == "(" ) {
			out += "idPow";
		} else if ( main != nullptr && i == main->open - 1 ) {
			out += "idMain";
		} else if ( main != nullptr && i > main->open && i < mainClose && t.text == "out" ) {
			out += "inout";
		} else {
			out += mapped;
		}
	}
}

// The sampler parameters of the function whose parameters open at tokens[open].
[[nodiscard]] std::vector<idStr> SamplerParameters( const tokens_t & tokens, int open ) {
	std::vector<idStr> samplers;
	const int close = Match( tokens, open );
	for ( int i = open + 1; i + 1 < close; i++ ) {
		if ( ( tokens[i].text == "sampler2D" || tokens[i].text == "samplerCUBE" ) && tokens[i + 1].type == TT_NAME ) {
			samplers.push_back( tokens[i + 1].text );
		}
	}
	return samplers;
}

// The members of VS_IN the body of main reads; all of them when it uses the struct whole.
void ReadMembers( const tokens_t & tokens, const decl_t & main, const idStr & input, std::vector<idStr> & read, bool & all ) {
	for ( int i = Match( tokens, main.open ) + 1; i < main.end; i++ ) {
		if ( tokens[i].text != input || tokens[i - 1].text == "." ) {
			continue;
		}
		if ( i + 2 < main.end && tokens[i + 1].text == "." && tokens[i + 2].type == TT_NAME ) {
			if ( std::ranges::find( read, tokens[i + 2].text ) == read.end() ) {
				read.push_back( tokens[i + 2].text );
			}
		} else {
			all = true;
		}
	}
}

// The leading line breaks of a declaration's whitespace go: Emit writes one before each.
[[nodiscard]] idStr TrimLeadingLines( const idStr & space ) {
	int skip = 0;
	while ( skip < space.Length() && ( space[skip] == '\n' || space[skip] == '\r' ) ) {
		skip++;
	}
	return idStr( space.c_str() + skip );
}

void NewLine( idStr & out ) {
	if ( out.Length() > 0 && out[out.Length() - 1] != '\n' ) {
		out += '\n';
	}
}

}	// namespace

std::expected<d3d12EmittedStage_t, idStr> idD3D12HlslEmitter::Emit( shaderStageType_t stage, const renderProgSource_t & source, const char * name ) {
	const bool vertex = ( stage == shaderStageType_t::VERTEX );
	const tokens_t tokens = Tokenize( source.text, name );
	for ( size_t i = 0; i < tokens.size(); i++ ) {
		if ( tokens[i].text == "#" ) {
			const char * directive = ( i + 1 < tokens.size() ) ? tokens[i + 1].text.c_str() : "";
			return Fail( name, va( "a preprocessor line #%s is left: StripDeadCode could not parse the source", directive ) );
		}
	}
	auto split = SplitDeclarations( tokens, name );
	if ( !split ) {
		return unexpected_t( split.error() );
	}
	std::vector<decl_t> & decls = *split;

	d3d12EmittedStage_t emitted;
	std::vector<idStr> parms;
	std::vector<idStr> samplers;
	std::array<entryStruct_t, static_cast<size_t>( entry_t::COUNT )> structs;
	const decl_t * main = nullptr;

	// classify; the structs and the parms first, the text needs all of them
	for ( decl_t & decl : decls ) {
		const auto At = [&]( int k ) -> const idStr & {
			static const idStr none;
			return ( decl.begin + k < decl.end ) ? tokens[decl.begin + k].text : none;
		};
		if ( At( 0 ) == "uniform" ) {
			if ( At( 1 ) == "float4" && decl.begin + 2 < decl.end && tokens[decl.begin + 2].type == TT_NAME ) {
				decl.kind = declKind_t::PARM;
				parms.push_back( At( 2 ) );
			} else if ( At( 2 ) == "{" ) {
				if ( At( 1 ) != "matrices_ubo" ) {
					return Fail( name, va( "uniform block %s: only matrices_ubo is bound", At( 1 ).c_str() ) );
				}
				decl.kind = declKind_t::JOINTS;
				emitted.usesJoints = true;
			} else if ( At( 1 ) == "sampler2D" || At( 1 ) == "samplerCUBE" ) {
				const idStr & sampler = At( 2 );
				const int slot = ( sampler.Cmpn( "samp", 4 ) == 0 && sampler.Length() > 4 && idStr::IsNumeric( sampler.c_str() + 4 ) ) ? atoi( sampler.c_str() + 4 ) : -1;
				if ( slot < 0 || slot >= MAX_TEXTURE_SLOTS ) {
					return Fail( name, va( "sampler %s: the slot comes from the name sampN, N below %d", sampler.c_str(), MAX_TEXTURE_SLOTS ) );
				}
				decl.kind = declKind_t::SAMPLER;
				samplers.push_back( sampler );
			} else {
				return Fail( name, va( "uniform %s %s is not supported", At( 1 ).c_str(), At( 2 ).c_str() ) );
			}
		} else if ( const std::optional<entry_t> entry = EntryStruct( tokens, decl ) ) {
			entryStruct_t & s = structs[static_cast<size_t>( *entry )];
			auto members = ParseMembers( tokens, decl, name );
			if ( !members ) {
				return unexpected_t( members.error() );
			}
			s.declared = true;
			s.members = std::move( *members );
			decl.kind = declKind_t::ENTRY_STRUCT;
		} else if ( decl.open > 0 && tokens[decl.end - 1].text == "}" ) {
			decl.kind = ( tokens[decl.open - 1].text == "main" ) ? declKind_t::MAIN : declKind_t::FUNCTION;
			if ( decl.kind == declKind_t::MAIN ) {
				if ( main != nullptr ) {
					return Fail( name, "two functions main" );
				}
				main = &decl;
			}
		}
	}
	if ( main == nullptr ) {
		return Fail( name, "no function main" );
	}

	// E1: the parms are info.parms, in the order of the source's list
	if ( static_cast<int>( parms.size() ) != source.parms.Num() || source.parms.Num() > MAX_RENDER_PARMS ) {
		return Fail( name, va( "%d uniform float4 declarations for %d parms", static_cast<int>( parms.size() ), source.parms.Num() ) );
	}
	for ( int i = 0; i < source.parms.Num(); i++ ) {
		if ( parms[i] != R_RenderParmName( source.parms[i] ) ) {
			return Fail( name, va( "uniform %s is parm %d, the list says %s", parms[i].c_str(), i, R_RenderParmName( source.parms[i] ) ) );
		}
		emitted.info.parms[i] = static_cast<uint8>( source.parms[i] );
	}
	emitted.info.numParms = source.parms.Num();

	auto signature = ParseMain( tokens, *main, name );
	if ( !signature ) {
		return unexpected_t( signature.error() );
	}
	const entry_t input = vertex ? entry_t::VS_IN : entry_t::PS_IN;
	const entry_t output = vertex ? entry_t::VS_OUT : entry_t::PS_OUT;
	if ( signature->output != static_cast<int>( output ) || ( signature->input >= 0 && signature->input != static_cast<int>( input ) )
			|| ( vertex && signature->input < 0 ) ) {
		return Fail( name, vertex ? "main is not main( VS_IN, out VS_OUT )" : "main is not main( [PS_IN,] out PS_OUT )" );
	}
	emitted.hasInput = ( signature->input >= 0 );
	if ( ( emitted.hasInput && !structs[static_cast<size_t>( input )].declared ) || !structs[static_cast<size_t>( output )].declared ) {
		return Fail( name, "a struct of main's parameters is not declared" );
	}

	// the entry structs: E7, E8, E12
	idStr entryText[static_cast<size_t>( entry_t::COUNT )];
	const auto Member = []( const entryMember_t & m, const char * semantic ) {
		return idStr( va( "\t%s %s%s%s;\n", m.type.c_str(), m.name.c_str(), ( semantic != nullptr ) ? " : " : "", ( semantic != nullptr ) ? semantic : "" ) );
	};
	if ( vertex ) {
		std::vector<idStr> read;
		bool all = false;
		ReadMembers( tokens, *main, signature->inputName, read, all );
		idStr & text = entryText[static_cast<size_t>( entry_t::VS_IN )];
		for ( const entryMember_t & m : structs[static_cast<size_t>( entry_t::VS_IN )].members ) {
			if ( !all && std::ranges::find( read, m.name ) == read.end() ) {
				continue;
			}
			const int attrib = R_VertexAttribIndex( m.semantic.c_str() );
			if ( attrib < 0 ) {
				return Fail( name, va( "VS_IN member %s reads %s, which no vertex attribute provides", m.name.c_str(), m.written.c_str() ) );
			}
			emitted.attribs = static_cast<uint16>( emitted.attribs | ( 1 << attrib ) );
			text += Member( m, m.written.c_str() );
		}
		for ( const entryMember_t & m : structs[static_cast<size_t>( entry_t::VS_OUT )].members ) {
			const int bit = VaryingBit( m.semantic );
			const int components = Components( m.type );
			if ( m.semantic == "POSITION" && components == 4 && emitted.position.components == 0 ) {
				emitted.position = { m.name, components };
			} else if ( bit < 0 || components == 0 || ( emitted.varyings & ( 1 << bit ) ) != 0 ) {
				return Fail( name, va( "VS_OUT member %s %s : %s is not supported", m.type.c_str(), m.name.c_str(), m.written.c_str() ) );
			} else {
				emitted.varyings = static_cast<uint16>( emitted.varyings | ( 1 << bit ) );
				emitted.members[bit] = { m.name, components };
			}
			entryText[static_cast<size_t>( entry_t::VS_OUT )] += Member( m, nullptr );
		}
		if ( emitted.position.components == 0 ) {
			return Fail( name, "VS_OUT has no float4 POSITION" );
		}
	} else {
		for ( const entryMember_t & m : structs[static_cast<size_t>( entry_t::PS_IN )].members ) {
			const int bit = VaryingBit( m.semantic );
			const int components = Components( m.type );
			if ( ( m.semantic == "WPOS" || m.semantic == "VPOS" ) && components == 4 && emitted.position.components == 0 ) {
				emitted.position = { m.name, components };
			} else if ( bit < 0 || components == 0 || ( emitted.varyings & ( 1 << bit ) ) != 0 || m.name == "position" ) {
				return Fail( name, va( "PS_IN member %s %s : %s is not supported", m.type.c_str(), m.name.c_str(), m.written.c_str() ) );
			} else {
				emitted.varyings = static_cast<uint16>( emitted.varyings | ( 1 << bit ) );
				emitted.members[bit] = { m.name, components };
			}
			entryText[static_cast<size_t>( entry_t::PS_IN )] += Member( m, nullptr );
		}
		if ( emitted.hasInput && emitted.position.components == 0 ) {
			// gl_FragCoord, which GL gives every fragment stage
			emitted.position = { "position", 4 };
			entryText[static_cast<size_t>( entry_t::PS_IN )] = idStr( "\tfloat4 position;\n" ) + entryText[static_cast<size_t>( entry_t::PS_IN )];
		}
		for ( const entryMember_t & m : structs[static_cast<size_t>( entry_t::PS_OUT )].members ) {
			const int components = Components( m.type );
			const int target = ( m.semantic == "COLOR" ) ? 0 : ( m.semantic.Cmpn( "COLOR", 5 ) == 0 && m.semantic.Length() == 6 ) ? m.semantic[5] - '0' : -1;
			if ( m.semantic == "DEPTH" && components == 1 && !emitted.writesDepth ) {
				emitted.writesDepth = true;
				emitted.depth = { m.name, components };
			} else if ( target < 0 || target >= D3D12_MAX_TARGETS || components == 0 || emitted.targets[target].components != 0 ) {
				return Fail( name, va( "PS_OUT member %s %s : %s is not supported", m.type.c_str(), m.name.c_str(), m.written.c_str() ) );
			} else {
				emitted.targets[target] = { m.name, components };
			}
			entryText[static_cast<size_t>( entry_t::PS_OUT )] += Member( m, nullptr );
		}
	}

	// the text in source order
	idStr & out = emitted.body;
	out = PRELUDE;
	if ( std::ranges::any_of( tokens, []( const token_t & t ) { return t.type == TT_NAME && t.text == "pow"; } ) ) {
		out += POW_PRELUDE;
	}
	if ( !parms.empty() ) {
		out += vertex ? "cbuffer idVertexParms : register( b0 ) {\n" : "cbuffer idFragmentParms : register( b1 ) {\n";
		for ( const idStr & parm : parms ) {
			out += va( "\tfloat4 %s;\n", parm.c_str() );
		}
		out += "};\n";
	}
	scope_t globalScope;
	globalScope.globalSamplers = &samplers;
	for ( const decl_t & decl : decls ) {
		if ( decl.kind == declKind_t::PARM ) {
			continue;
		}
		NewLine( out );
		out += TrimLeadingLines( tokens[decl.begin].space );
		switch ( decl.kind ) {
			case declKind_t::JOINTS: {
				out += "cbuffer matrices_ubo : register( b2 )";
				out += tokens[decl.begin + 2].space;
				scope_t none;
				Rewrite( tokens, decl.begin + 2, decl.end, none, nullptr, out );
				break;
			}
			case declKind_t::SAMPLER: {
				const idStr & sampler = tokens[decl.begin + 2].text;
				const int slot = atoi( sampler.c_str() + 4 );
				out += va( "%s %s_t : register( t%d );\nSamplerState %s_s : register( s%d );", ( tokens[decl.begin + 1].text == "samplerCUBE" ) ? "TextureCube" : "Texture2D",
					sampler.c_str(), slot, sampler.c_str(), slot );
				break;
			}
			case declKind_t::ENTRY_STRUCT: {
				const entry_t entry = *EntryStruct( tokens, decl );
				out += va( "struct %s {\n%s};", ENTRY_NAMES[static_cast<int>( entry )], entryText[static_cast<size_t>( entry )].c_str() );
				break;
			}
			case declKind_t::FUNCTION:
			case declKind_t::MAIN: {
				scope_t scope;
				scope.globalSamplers = &samplers;
				scope.parameterSamplers = SamplerParameters( tokens, decl.open );
				Rewrite( tokens, decl.begin, decl.end, scope, ( decl.kind == declKind_t::MAIN ) ? &decl : nullptr, out );
				break;
			}
			default: {
				// E16: a global without static would be a uniform in $Globals, its initializer ignored
				const idStr & first = tokens[decl.begin].text;
				const bool prototype = ( decl.open >= 0 && std::none_of( tokens.begin() + decl.begin, tokens.begin() + decl.open, []( const token_t & t ) { return t.text == "="; } ) );
				if ( first != "static" && first != "struct" && first != "typedef" && !prototype ) {
					out += "static ";
				}
				Rewrite( tokens, decl.begin, decl.end, globalScope, nullptr, out );
				break;
			}
		}
	}
	NewLine( out );
	return emitted;
}

idStr idD3D12HlslEmitter::WithEntry( shaderStageType_t stage, const d3d12EmittedStage_t & emitted, uint16 varyings ) {
	idStr text = emitted.body;
	text += "struct idVaryings {\n\tfloat4 position : SV_Position;\n";
	for ( int bit = 0; bit < D3D12_NUM_VARYINGS; bit++ ) {
		if ( ( varyings & ( 1 << bit ) ) != 0 ) {
			text += va( "\tfloat4 %s : %s;\n", VaryingName( bit ).c_str(), VaryingSemantic( bit ).c_str() );
		}
	}
	text += "};\n";

	if ( stage == shaderStageType_t::VERTEX ) {
		// E7: a VS_IN without members is no input signature
		text += ( emitted.attribs != 0 ) ? "idVaryings vsMain( VS_IN vertex ) {\n" : "idVaryings vsMain() {\n\tVS_IN vertex = (VS_IN)0;\n";
		text += "\tVS_OUT result = (VS_OUT)0;\n\tidMain( vertex, result );\n";
		// E17: precise reaches back through the whole chain, so depth EQUAL passes between different vertex stages
		text += va( "\tprecise float4 clip = idGLClipToD3D( result.%s );\n\tidVaryings o;\n\to.position = clip;\n", emitted.position.name.c_str() );
		for ( int bit = 0; bit < D3D12_NUM_VARYINGS; bit++ ) {
			if ( ( varyings & ( 1 << bit ) ) == 0 ) {
				continue;
			}
			const d3d12StructMember_t & member = emitted.members[bit];
			idStr value = ( member.components > 0 ) ? Widen( idStr( "result." ) + member.name, member.components ) : idStr( "float4( 0.0, 0.0, 0.0, 0.0 )" );
			if ( bit >= D3D12_VARYING_COLOR0 && member.components > 0 ) {
				value = idStr( "saturate( " ) + value + " )";	// E11: GL's compatibility profile clamps gl_FrontColor
			}
			text += va( "\to.%s = %s;\n", VaryingName( bit ).c_str(), value.c_str() );
		}
		text += "\treturn o;\n}\n";
		return text;
	}

	int numTargets = 0;
	for ( const d3d12StructMember_t & target : emitted.targets ) {
		numTargets += ( target.components > 0 ) ? 1 : 0;
	}
	const bool single = ( numTargets == 1 && emitted.targets[0].components > 0 && !emitted.writesDepth );
	if ( single ) {
		text += va( "%s psMain( idVaryings v ) : SV_Target0 {\n", FloatType( emitted.targets[0].components ) );
	} else {
		text += "struct idTargets {\n";
		for ( int i = 0; i < D3D12_MAX_TARGETS; i++ ) {
			if ( emitted.targets[i].components > 0 ) {
				text += va( "\t%s %s : SV_Target%d;\n", FloatType( emitted.targets[i].components ), emitted.targets[i].name.c_str(), i );
			}
		}
		if ( emitted.writesDepth ) {
			text += va( "\tfloat %s : SV_Depth;\n", emitted.depth.name.c_str() );	// E12
		}
		text += "};\nidTargets psMain( idVaryings v ) {\n";
	}
	if ( emitted.hasInput ) {
		// E10: gl_FragCoord, w included
		text += va( "\tPS_IN fragment;\n\tfragment.%s = float4( v.position.xyz, 1.0 / v.position.w );\n", emitted.position.name.c_str() );
		for ( int bit = 0; bit < D3D12_NUM_VARYINGS; bit++ ) {
			const d3d12StructMember_t & member = emitted.members[bit];
			if ( member.components == 0 ) {
				continue;
			}
			if ( ( varyings & ( 1 << bit ) ) != 0 ) {
				text += va( "\tfragment.%s = v.%s%s;\n", member.name.c_str(), VaryingName( bit ).c_str(), Narrow( member.components ) );
			} else {
				text += va( "\tfragment.%s = 0.0;\n", member.name.c_str() );
			}
		}
	}
	text += "\tPS_OUT result = (PS_OUT)0;\n";
	text += emitted.hasInput ? "\tidMain( fragment, result );\n" : "\tidMain( result );\n";
	if ( single ) {
		text += va( "\treturn result.%s;\n}\n", emitted.targets[0].name.c_str() );
		return text;
	}
	text += "\tidTargets o;\n";
	for ( const d3d12StructMember_t & target : emitted.targets ) {
		if ( target.components > 0 ) {
			text += va( "\to.%s = result.%s;\n", target.name.c_str(), target.name.c_str() );
		}
	}
	if ( emitted.writesDepth ) {
		text += va( "\to.%s = result.%s;\n", emitted.depth.name.c_str(), emitted.depth.name.c_str() );
	}
	text += "\treturn o;\n}\n";
	return text;
}
