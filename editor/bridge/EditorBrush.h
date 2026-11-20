// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.
#pragma once

/*
	Brushes as the editor needs them: geometry made from the planes of an idMapBrush, without a
	map compiler.

	  idMapBrush (planes, local to its entity's origin)
	        │ BrushWindings: every plane clipped by the others
	        ├── BuildBrushModel ── a render model, texture coordinates as dmap computes them
	        ├── RayBrush ───────── which brush and which face a click hits
	        └── MoveFace, Hollow, MakeBox, ... ── the edits, each checked with BrushWindings

	A plane is n·x + d = 0 with the normal pointing out of the brush. The texture of a side is
	laid out in world space: st = ( local + entity origin ) · axis base · texMat.
*/

// how the editor shows a brush
enum class editorBrushKind_t {
	DRAWN,			// at least one side has a material the game draws
	HIDDEN,			// caulk, nodraw: nothing to see
	PORTAL,			// a side with an area portal material
	CLIP,			// player, monster and other clip brushes
	TRIGGER
};

// a side without a face (cut away by the others) has a winding of no points
using editorBrushWindings_t = idList<idFixedWinding>;

namespace editorBrush {

// false when the brush encloses no volume
[[nodiscard]] bool	Windings( const idMapBrush & brush, editorBrushWindings_t & windings );
[[nodiscard]] int	NumFaces( const editorBrushWindings_t & windings );
[[nodiscard]] idBounds	Bounds( const editorBrushWindings_t & windings );
[[nodiscard]] editorBrushKind_t	Kind( const idMapBrush & brush );

// The texture vectors of a side for an entity at origin, as dmap makes them: st = xyz · v + v[3]
// for xyz local to the entity.
void				TextureVectors( const idMapBrushSide & side, const idVec3 & origin, idVec4 v[2] );

// Where a ray in the brush's space enters it; false when it misses or starts inside.
[[nodiscard]] bool	Ray( const idMapBrush & brush, const idVec3 & start, const idVec3 & dir, float & distance, int & side );

[[nodiscard]] bool	PatchIsDrawn( const idMapPatch & patch );
[[nodiscard]] idBounds	PatchBounds( const idMapPatch & patch );
// mesh, a copy of the patch, becomes the triangles the game makes of it
[[nodiscard]] bool	Tessellate( const idMapPatch & patch, idSurface_Patch & mesh );
// the nearest triangle of the patch on the ray
[[nodiscard]] bool	RayPatch( const idMapPatch & patch, const idVec3 & start, const idVec3 & dir, float & distance );

// An axial box; the caller owns it. repeat: world units per repeat of the texture.
// An area portal material goes on one of the two largest sides only, the others get nodraw.
[[nodiscard]] idMapBrush *	MakeBox( const idBounds & bounds, const char * material, float repeat, const idVec3 & origin );
void				Translate( idMapBrush & brush, const idVec3 & delta );
// A turn around a world axis as rows: v * matrix is v turned. Exact for multiples of 90 degrees.
[[nodiscard]] idMat3	AxisRotation( int axis, float degrees );
// Turns the brush around pivot (in its entity's space, origin is the entity's). The texture turns
// with it. A quarter turn of an axial side only swaps numbers, so four of them change nothing.
void				Rotate( idMapBrush & brush, const idMat3 & rotation, const idVec3 & pivot, const idVec3 & origin );
// all control points; the texture coordinates are the points' own and stay
void				TranslatePatch( idMapPatch & patch, const idVec3 & delta );
void				RotatePatch( idMapPatch & patch, const idMat3 & rotation, const idVec3 & pivot );
// Pushes a side out along its normal (in for a negative distance). false and no change when the
// brush would lose a face or all of its volume.
[[nodiscard]] bool	MoveFace( idMapBrush & brush, int side, float distance );
// One wall for every face, thickness deep; false and no walls when the brush is too small for it.
[[nodiscard]] bool	Hollow( const idMapBrush & brush, float thickness, idList<idMapBrush *> & walls );
// between an entity at oldOrigin and one at newOrigin, staying where it is in the world, texture too
void				Rebase( idMapBrush & brush, const idVec3 & oldOrigin, const idVec3 & newOrigin );

// repeat: world units per repeat along S and T; shift in repeats; rotate in degrees
void				SetTexture( idMapBrushSide & side, float shiftS, float shiftT, float repeatS, float repeatT, float rotate );
void				GetTexture( const idMapBrushSide & side, float & shiftS, float & shiftT, float & repeatS, float & repeatT, float & rotate );

}	// namespace editorBrush

// One render model of many primitives: a surface for every material, in the entity's space.
class idEditorModelBuilder {
public:
						~idEditorModelBuilder();

	// the sides with a material the game draws; origin: of the brush's entity, for the texture
	void				AddBrush( const idMapBrush & brush, const editorBrushWindings_t & windings, const idVec3 & origin );
	void				AddPatch( const idMapPatch & patch );
	// nullptr when nothing was added; the caller frees the model with renderModelManager->FreeModel
	[[nodiscard]] idRenderModel *	Finish();

private:
	// triIndex_t is 16 bits
	static constexpr int	MAX_SURFACE_VERTS = 60000;

	struct group_t {
		const idMaterial *	material = nullptr;
		idList<idDrawVert>	verts;
		idList<triIndex_t>	indexes;
	};
	[[nodiscard]] group_t &	Group( const idMaterial * material, int numVerts );

	idList<group_t *>	groups;
};
