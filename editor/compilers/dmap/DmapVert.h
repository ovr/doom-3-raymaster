/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company. 
Copyright (C) 2026 Dmitry Patsura

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").  

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#ifndef __DMAPVERT_H__
#define __DMAPVERT_H__

/*
===============================================================================

	The map compiler's vertex: the full float draw vertex of the original engine.
	BFG's idDrawVert packs the texture coordinates and the normal.

===============================================================================
*/

struct dmapVert_t {
	idVec3			xyz;
	idVec2			st;
	idVec3			normal;
	idVec3			tangents[2];
	byte			color[4];
	float			operator[]( const int index ) const;
	float &			operator[]( const int index );

	void			Clear();

	void			Lerp( const dmapVert_t &a, const dmapVert_t &b, const float f );
	void			LerpAll( const dmapVert_t &a, const dmapVert_t &b, const float f );

					// the packed vertex of a model or a map patch
	void			FromDrawVert( const idDrawVert &dv );

};

ID_INLINE float dmapVert_t::operator[]( const int index ) const {
	assert( index >= 0 && index < 5 );
	return ((float *)(&xyz))[index];
}
ID_INLINE float	&dmapVert_t::operator[]( const int index ) {
	assert( index >= 0 && index < 5 );
	return ((float *)(&xyz))[index];
}

ID_INLINE void dmapVert_t::Clear() {
	xyz.Zero();
	st.Zero();
	normal.Zero();
	tangents[0].Zero();
	tangents[1].Zero();
	color[0] = color[1] = color[2] = color[3] = 0;
}

ID_INLINE void dmapVert_t::Lerp( const dmapVert_t &a, const dmapVert_t &b, const float f ) {
	xyz = a.xyz + f * ( b.xyz - a.xyz );
	st = a.st + f * ( b.st - a.st );
}

ID_INLINE void dmapVert_t::LerpAll( const dmapVert_t &a, const dmapVert_t &b, const float f ) {
	xyz = a.xyz + f * ( b.xyz - a.xyz );
	st = a.st + f * ( b.st - a.st );
	normal = a.normal + f * ( b.normal - a.normal );
	tangents[0] = a.tangents[0] + f * ( b.tangents[0] - a.tangents[0] );
	tangents[1] = a.tangents[1] + f * ( b.tangents[1] - a.tangents[1] );
	color[0] = (byte)( a.color[0] + f * ( b.color[0] - a.color[0] ) );
	color[1] = (byte)( a.color[1] + f * ( b.color[1] - a.color[1] ) );
	color[2] = (byte)( a.color[2] + f * ( b.color[2] - a.color[2] ) );
	color[3] = (byte)( a.color[3] + f * ( b.color[3] - a.color[3] ) );
}

ID_INLINE void dmapVert_t::FromDrawVert( const idDrawVert &dv ) {
	Clear();
	xyz = dv.xyz;
	st = dv.GetTexCoord();
	normal = dv.GetNormal();
	tangents[0] = dv.GetTangent();
	tangents[1] = dv.GetBiTangent();
	memcpy( color, dv.color, sizeof( color ) );
}

#endif /* !__DMAPVERT_H__ */
