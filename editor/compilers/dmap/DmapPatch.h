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

#ifndef __DMAPPATCH_H__
#define __DMAPPATCH_H__

/*
===============================================================================

	Bezier patch surface on float vertices: idSurface_Patch of the original engine,
	reduced to what the map compiler needs.

===============================================================================
*/

class dmapPatch_t {

public:
						// the control points of a map patch
	explicit			dmapPatch_t( const idSurface_Patch &patch );

	const dmapVert_t &	operator[]( const int index ) const { return verts[index]; }
	void				SetTexCoord( int index, const idVec2 &st ) { verts[index].st = st; }
	int					GetNumIndexes() const { return indexes.Num(); }
	const int *			GetIndexes() const { return indexes.Ptr(); }
	int					GetNumVertices() const { return verts.Num(); }

	void				SetSize( int patchWidth, int patchHeight );
	int					GetWidth() const;
	int					GetHeight() const;

						// subdivide the patch mesh based on error
	void				Subdivide( float maxHorizontalError, float maxVerticalError, float maxLength, bool genNormals = false );
						// subdivide the patch up to an explicit number of horizontal and vertical subdivisions
	void				SubdivideExplicit( int horzSubdivisions, int vertSubdivisions, bool genNormals, bool removeLinear = false );

protected:
	idList<dmapVert_t>	verts;			// vertices
	idList<int>			indexes;		// 3 references to vertices for each triangle
	int					width;			// width of patch
	int					height;			// height of patch
	int					maxWidth;		// maximum width allocated for
	int					maxHeight;		// maximum height allocated for
	bool				expanded;		// true if vertices are spaced out

private:
						// put the approximation points on the curve
	void				PutOnCurve();
						// remove columns and rows with all points on one line
	void				RemoveLinearColumnsRows();
						// resize verts buffer
	void				ResizeExpanded( int height, int width );
						// space points out over maxWidth * maxHeight buffer
	void				Expand();
						// move all points to the start of the verts buffer
	void				Collapse();
						// project a point onto a vector to calculate maximum curve error
	void				ProjectPointOntoVector( const idVec3 &point, const idVec3 &vStart, const idVec3 &vEnd, idVec3 &vProj );
						// generate normals
	void				GenerateNormals();
						// generate triangle indexes
	void				GenerateIndexes();
						// lerp point from two patch point
	void				LerpVert( const dmapVert_t &a, const dmapVert_t &b, dmapVert_t &out ) const;
						// sample a single 3x3 patch
	void				SampleSinglePatchPoint( const dmapVert_t ctrl[3][3], float u, float v, dmapVert_t *out ) const;
	void				SampleSinglePatch( const dmapVert_t ctrl[3][3], int baseCol, int baseRow, int width, int horzSub, int vertSub, dmapVert_t *outVerts ) const;
};

/*
=================
dmapPatch_t::dmapPatch_t
=================
*/
ID_INLINE dmapPatch_t::dmapPatch_t( const idSurface_Patch &patch ) {
	width = maxWidth = patch.GetWidth();
	height = maxHeight = patch.GetHeight();
	expanded = false;
	verts.SetNum( width * height );
	for ( int i = 0; i < verts.Num(); i++ ) {
		verts[i].FromDrawVert( patch[i] );
	}
}

/*
=================
dmapPatch_t::GetWidth
=================
*/
ID_INLINE int dmapPatch_t::GetWidth() const {
	return width;
}

/*
=================
dmapPatch_t::GetHeight
=================
*/
ID_INLINE int dmapPatch_t::GetHeight() const {
	return height;
}

#endif /* !__DMAPPATCH_H__ */
