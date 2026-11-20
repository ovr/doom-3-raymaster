// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.
#pragma once

#include <string>
#include <string_view>
#include <vector>

/*
	The text of one material declaration as a tree of lines, for editing it by key.

	  textures/base_wall/lfwall13f3          <- the header: [type] name
	  {                                      <- body.open
	      qer_editorimage  textures/...      <- a line: key, value
	      noShadows                          <- a line without a value
	      {                                  <- a block (a stage) with lines of its own
	          blend  bumpmap
	          map    addnormals( a, b )
	      }
	  }                                      <- body.close

	Nothing is rewritten: the tree only points into the text (byte offsets), so the comments, the
	spacing and the line ends stay as they are and an edit replaces the bytes of one value.
	A line is what stands between two line ends or braces; a comment is no part of it.
	The header includes neither the engine's headers nor Qt's: both halves of the editor use it.
*/
namespace mtr {

struct line_t {
	size_t					begin = 0;			// of the key
	size_t					end = 0;			// after the value, or after the key when there is none
	size_t					valueBegin = 0;		// == end when there is no value
	std::string				key;
};

struct block_t {
	size_t					open = 0;			// the offset of '{'
	size_t					close = 0;			// the offset of '}'
	std::vector<line_t>		lines;
	std::vector<block_t>	blocks;
};

struct decl_t {
	std::string				type;				// "" when the text names none: a material
	std::string				name;
	block_t					body;
};

namespace detail {

[[nodiscard]] inline bool IsSpace( char c ) {
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// the end of the comment that starts at pos, or pos when none does
[[nodiscard]] inline size_t SkipComment( std::string_view text, size_t pos ) {
	if ( pos + 1 >= text.size() || text[pos] != '/' ) {
		return pos;
	}
	if ( text[pos + 1] == '/' ) {
		const size_t lineEnd = text.find( '\n', pos );
		return ( lineEnd == std::string_view::npos ) ? text.size() : lineEnd;
	}
	if ( text[pos + 1] == '*' ) {
		const size_t close = text.find( "*/", pos + 2 );
		return ( close == std::string_view::npos ) ? text.size() : close + 2;
	}
	return pos;
}

// Reads the line that starts at pos, a non-space character that is no brace and no comment.
inline void ReadLine( std::string_view text, size_t & pos, line_t & line ) {
	line.begin = pos;
	size_t keyEnd = std::string_view::npos;
	size_t last = pos;		// after the last character that is no space
	while ( pos < text.size() ) {
		const char c = text[pos];
		if ( c == '\n' || c == '{' || c == '}' || SkipComment( text, pos ) != pos ) {
			break;
		}
		if ( c == '"' ) {
			const size_t close = text.find_first_of( "\"\n", pos + 1 );
			pos = ( close == std::string_view::npos ) ? text.size() : ( text[close] == '"' ? close + 1 : close );
			last = pos;
			continue;
		}
		if ( IsSpace( c ) ) {
			if ( keyEnd == std::string_view::npos ) {
				keyEnd = pos;
			}
		} else {
			last = pos + 1;
		}
		pos++;
	}
	if ( keyEnd == std::string_view::npos || keyEnd > last ) {
		keyEnd = last;
	}
	line.end = last;
	line.key.assign( text.substr( line.begin, keyEnd - line.begin ) );
	line.valueBegin = keyEnd;
	while ( line.valueBegin < last && IsSpace( text[line.valueBegin] ) ) {
		line.valueBegin++;
	}
}

// pos is after the block's '{'; on return it is after its '}'
[[nodiscard]] inline bool ReadBlock( std::string_view text, size_t & pos, block_t & block, int depth ) {
	constexpr int MAX_DEPTH = 64;
	while ( pos < text.size() ) {
		const char c = text[pos];
		const size_t afterComment = SkipComment( text, pos );
		if ( afterComment != pos ) {
			pos = afterComment;
		} else if ( IsSpace( c ) ) {
			pos++;
		} else if ( c == '}' ) {
			block.close = pos++;
			return true;
		} else if ( c == '{' ) {
			if ( depth >= MAX_DEPTH ) {
				return false;
			}
			block_t & child = block.blocks.emplace_back();
			child.open = pos++;
			if ( !ReadBlock( text, pos, child, depth + 1 ) ) {
				return false;
			}
		} else {
			ReadLine( text, pos, block.lines.emplace_back() );
		}
	}
	return false;
}

}	// namespace detail

// false: the braces do not match, or there is no body; error says which
[[nodiscard]] inline bool ParseDecl( std::string_view text, decl_t & decl, std::string & error ) {
	decl = decl_t();
	size_t pos = 0;
	std::vector<std::string> header;
	while ( pos < text.size() && text[pos] != '{' ) {
		const size_t afterComment = detail::SkipComment( text, pos );
		if ( afterComment != pos ) {
			pos = afterComment;
		} else if ( detail::IsSpace( text[pos] ) ) {
			pos++;
		} else {
			const size_t begin = pos;
			while ( pos < text.size() && !detail::IsSpace( text[pos] ) && text[pos] != '{' && detail::SkipComment( text, pos ) == pos ) {
				pos++;
			}
			header.emplace_back( text.substr( begin, pos - begin ) );
		}
	}
	if ( pos >= text.size() ) {
		error = "the declaration has no { body }";
		return false;
	}
	if ( header.empty() || header.size() > 2 ) {
		error = "the declaration starts with neither 'name {' nor 'type name {'";
		return false;
	}
	decl.name = header.back();
	if ( header.size() == 2 ) {
		decl.type = header.front();
	}
	decl.body.open = pos++;
	if ( !detail::ReadBlock( text, pos, decl.body, 0 ) ) {
		error = "a { has no matching }";
		return false;
	}
	for ( ; pos < text.size(); pos++ ) {
		const size_t afterComment = detail::SkipComment( text, pos );
		if ( afterComment != pos ) {
			pos = afterComment - 1;
		} else if ( !detail::IsSpace( text[pos] ) ) {
			error = "there is text after the closing }";
			return false;
		}
	}
	return true;
}

[[nodiscard]] inline std::string_view Value( std::string_view text, const line_t & line ) {
	return text.substr( line.valueBegin, line.end - line.valueBegin );
}

// The text with the line's value replaced; an empty value leaves the key alone on its line.
[[nodiscard]] inline std::string WithValue( std::string_view text, const line_t & line, std::string_view value ) {
	std::string result;
	result.reserve( text.size() + value.size() );
	if ( line.valueBegin < line.end ) {
		// keep the spacing between the key and the old value
		result.append( text.substr( 0, value.empty() ? line.begin + line.key.size() : line.valueBegin ) );
	} else {
		result.append( text.substr( 0, line.end ) );
		if ( !value.empty() ) {
			result.push_back( '\t' );
		}
	}
	result.append( value );
	result.append( text.substr( line.end ) );
	return result;
}

// the first line of the block with the key, compared without case; nullptr when there is none
[[nodiscard]] inline const line_t * FindLine( const block_t & block, std::string_view key ) {
	for ( const line_t & line : block.lines ) {
		if ( line.key.size() != key.size() ) {
			continue;
		}
		bool same = true;
		for ( size_t i = 0; i < key.size() && same; i++ ) {
			const char a = line.key[i], b = key[i];
			same = ( ( a >= 'A' && a <= 'Z' ) ? a + 32 : a ) == ( ( b >= 'A' && b <= 'Z' ) ? b + 32 : b );
		}
		if ( same ) {
			return &line;
		}
	}
	return nullptr;
}

}	// namespace mtr
