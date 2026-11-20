// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "idlib/precompiled.h"

#include "renderer/Model.h"

#include "EditorBrush.h"

namespace {

constexpr float		CLIP_EPSILON = 0.01f;
constexpr float		MIN_FACE_AREA = 0.01f;
constexpr float		RAY_PARALLEL = 1.0e-6f;
constexpr char		NODRAW_MATERIAL[] = "textures/common/nodraw";
constexpr int		CLIP_CONTENTS = CONTENTS_PLAYERCLIP | CONTENTS_MONSTERCLIP | CONTENTS_MOVEABLECLIP | CONTENTS_IKCLIP;

void SetVert( idDrawVert & vert, const idVec3 & xyz, const idVec3 & normal, float s, float t ) {
	vert.Clear();
	vert.xyz = xyz;
	vert.SetNormal( normal );
	vert.SetTexCoord( s, t );
	vert.color[0] = vert.color[1] = vert.color[2] = vert.color[3] = 255;
}

}	// namespace

bool editorBrush::Windings( const idMapBrush & brush, editorBrushWindings_t & windings ) {
	const int numSides = brush.GetNumSides();
	windings.SetNum( numSides );
	int faces = 0;
	for ( int i = 0; i < numSides; i++ ) {
		const idPlane & plane = brush.GetSide( i )->GetPlane();
		idFixedWinding & winding = windings[i];
		winding.BaseForPlane( plane );
		for ( int j = 0; j < numSides && winding.GetNumPoints() > 0; j++ ) {
			if ( j != i && !winding.ClipInPlace( -brush.GetSide( j )->GetPlane(), CLIP_EPSILON ) ) {
				winding.Clear();
			}
		}
		if ( winding.GetNumPoints() >= 3 && winding.GetArea() >= MIN_FACE_AREA ) {
			faces++;
		} else {
			winding.Clear();
		}
	}
	if ( faces < 4 ) {
		return false;
	}
	// a brush that is open to one side reaches the end of the world
	const idBounds bounds = Bounds( windings );
	for ( int axis = 0; axis < 3; axis++ ) {
		if ( bounds[0][axis] <= MIN_WORLD_COORD + 1.0f || bounds[1][axis] >= MAX_WORLD_COORD - 1.0f ) {
			return false;
		}
	}
	return true;
}

int editorBrush::NumFaces( const editorBrushWindings_t & windings ) {
	int faces = 0;
	for ( int i = 0; i < windings.Num(); i++ ) {
		faces += ( windings[i].GetNumPoints() >= 3 ) ? 1 : 0;
	}
	return faces;
}

idBounds editorBrush::Bounds( const editorBrushWindings_t & windings ) {
	idBounds bounds;
	bounds.Clear();
	for ( int i = 0; i < windings.Num(); i++ ) {
		for ( int j = 0; j < windings[i].GetNumPoints(); j++ ) {
			bounds.AddPoint( windings[i][j].ToVec3() );
		}
	}
	return bounds;
}

editorBrushKind_t editorBrush::Kind( const idMapBrush & brush ) {
	bool drawn = false;
	int contents = 0;
	for ( int i = 0; i < brush.GetNumSides(); i++ ) {
		const idMaterial * const material = declManager->FindMaterial( brush.GetSide( i )->GetMaterial() );
		if ( ( material->GetContentFlags() & CONTENTS_AREAPORTAL ) != 0 ) {
			return editorBrushKind_t::PORTAL;
		}
		drawn |= material->IsDrawn();
		contents |= material->GetContentFlags();
	}
	if ( drawn ) {
		return editorBrushKind_t::DRAWN;
	}
	if ( ( contents & CONTENTS_TRIGGER ) != 0 ) {
		return editorBrushKind_t::TRIGGER;
	}
	if ( ( contents & CLIP_CONTENTS ) != 0 && ( contents & CONTENTS_SOLID ) == 0 ) {
		return editorBrushKind_t::CLIP;
	}
	return editorBrushKind_t::HIDDEN;
}

void editorBrush::TextureVectors( const idMapBrushSide & side, const idVec3 & origin, idVec4 v[2] ) {
	idMapBrushSide moved = side;
	moved.SetOrigin( origin );
	moved.GetTextureVectors( v );
	// as dmap: the whole repeats of the offset say nothing and cost precision
	v[0][3] -= idMath::Floor( v[0][3] );
	v[1][3] -= idMath::Floor( v[1][3] );
}

bool editorBrush::Ray( const idMapBrush & brush, const idVec3 & start, const idVec3 & dir, float & distance, int & side ) {
	float enter = -idMath::INFINITY;
	float leave = idMath::INFINITY;
	int enterSide = -1;
	for ( int i = 0; i < brush.GetNumSides(); i++ ) {
		const idPlane & plane = brush.GetSide( i )->GetPlane();
		const float height = plane.Distance( start );
		const float approach = plane.Normal() * dir;
		if ( idMath::Fabs( approach ) < RAY_PARALLEL ) {
			if ( height > 0.0f ) {
				return false;
			}
			continue;
		}
		const float at = -height / approach;
		if ( approach < 0.0f ) {
			if ( at > enter ) {
				enter = at;
				enterSide = i;
			}
		} else {
			leave = Min( leave, at );
		}
		if ( enter > leave ) {
			return false;
		}
	}
	if ( enterSide == -1 || enter < 0.0f ) {
		return false;
	}
	distance = enter;
	side = enterSide;
	return true;
}

idEditorModelBuilder::~idEditorModelBuilder() {
	groups.DeleteContents( true );
}

idEditorModelBuilder::group_t & idEditorModelBuilder::Group( const idMaterial * material, int numVerts ) {
	for ( int i = 0; i < groups.Num(); i++ ) {
		if ( groups[i]->material == material && groups[i]->verts.Num() + numVerts <= MAX_SURFACE_VERTS ) {
			return *groups[i];
		}
	}
	group_t * const group = new ( TAG_IDLIB ) group_t;
	group->material = material;
	groups.Append( group );
	return *group;
}

void idEditorModelBuilder::AddBrush( const idMapBrush & brush, const editorBrushWindings_t & windings, const idVec3 & origin ) {
	for ( int i = 0; i < windings.Num(); i++ ) {
		const idFixedWinding & winding = windings[i];
		const int numPoints = winding.GetNumPoints();
		if ( numPoints < 3 ) {
			continue;
		}
		const idMapBrushSide & side = *brush.GetSide( i );
		const idMaterial * const material = declManager->FindMaterial( side.GetMaterial() );
		if ( !material->IsDrawn() ) {
			continue;
		}
		group_t & group = Group( material, numPoints );
		idVec4 texVec[2];
		editorBrush::TextureVectors( side, origin, texVec );
		const int first = group.verts.Num();
		for ( int j = 0; j < numPoints; j++ ) {
			const idVec3 & xyz = winding[j].ToVec3();
			SetVert( group.verts.Alloc(), xyz, side.GetPlane().Normal(), xyz * texVec[0].ToVec3() + texVec[0][3], xyz * texVec[1].ToVec3() + texVec[1][3] );
		}
		for ( int j = 2; j < numPoints; j++ ) {
			group.indexes.Append( static_cast<triIndex_t>( first ) );
			group.indexes.Append( static_cast<triIndex_t>( first + j - 1 ) );
			group.indexes.Append( static_cast<triIndex_t>( first + j ) );
		}
	}
}

void idEditorModelBuilder::AddPatch( const idMapPatch & patch ) {
	idSurface_Patch mesh( patch );
	if ( !editorBrush::PatchIsDrawn( patch ) || !editorBrush::Tessellate( patch, mesh ) || mesh.GetNumVertices() > MAX_SURFACE_VERTS ) {
		return;
	}
	group_t & group = Group( declManager->FindMaterial( patch.GetMaterial() ), mesh.GetNumVertices() );
	const int first = group.verts.Num();
	for ( int i = 0; i < mesh.GetNumVertices(); i++ ) {
		idDrawVert & vert = group.verts.Alloc();
		vert = mesh[i];
		vert.color[0] = vert.color[1] = vert.color[2] = vert.color[3] = 255;
	}
	for ( int i = 0; i < mesh.GetNumIndexes(); i++ ) {
		group.indexes.Append( static_cast<triIndex_t>( first + mesh.GetIndexes()[i] ) );
	}
}

idRenderModel * idEditorModelBuilder::Finish() {
	if ( groups.Num() == 0 ) {
		return nullptr;
	}
	idRenderModel * const model = renderModelManager->AllocModel();
	model->InitEmpty( "_editorGeometry" );
	for ( int i = 0; i < groups.Num(); i++ ) {
		const group_t & group = *groups[i];
		srfTriangles_t * const tri = model->AllocSurfaceTriangles( group.verts.Num(), group.indexes.Num() );
		memcpy( tri->verts, group.verts.Ptr(), group.verts.Num() * sizeof( idDrawVert ) );
		memcpy( tri->indexes, group.indexes.Ptr(), group.indexes.Num() * sizeof( triIndex_t ) );
		tri->numVerts = group.verts.Num();
		tri->numIndexes = group.indexes.Num();

		modelSurface_t surface = {};
		surface.shader = group.material;
		surface.geometry = tri;
		model->AddSurface( surface );
	}
	groups.DeleteContents( true );
	model->FinishSurfaces();
	return model;
}

bool editorBrush::PatchIsDrawn( const idMapPatch & patch ) {
	return patch.GetWidth() >= 3 && patch.GetHeight() >= 3 && declManager->FindMaterial( patch.GetMaterial() )->IsDrawn();
}

idBounds editorBrush::PatchBounds( const idMapPatch & patch ) {
	// the surface stays inside its control points
	idBounds bounds;
	bounds.Clear();
	for ( int i = 0; i < patch.GetNumVertices(); i++ ) {
		bounds.AddPoint( patch[i].xyz );
	}
	return bounds;
}

bool editorBrush::Tessellate( const idMapPatch & patch, idSurface_Patch & mesh ) {
	if ( patch.GetExplicitlySubdivided() ) {
		mesh.SubdivideExplicit( patch.GetHorzSubdivisions(), patch.GetVertSubdivisions(), true );
	} else {
		mesh.Subdivide( DEFAULT_CURVE_MAX_ERROR, DEFAULT_CURVE_MAX_ERROR, DEFAULT_CURVE_MAX_LENGTH, true );
	}
	return mesh.GetNumVertices() >= 3 && mesh.GetNumIndexes() >= 3;
}

bool editorBrush::RayPatch( const idMapPatch & patch, const idVec3 & start, const idVec3 & dir, float & distance ) {
	idSurface_Patch mesh( patch );
	if ( !Tessellate( patch, mesh ) ) {
		return false;
	}
	bool hit = false;
	const int * const indexes = mesh.GetIndexes();
	for ( int i = 0; i + 2 < mesh.GetNumIndexes(); i += 3 ) {
		const idVec3 & a = mesh[indexes[i]].xyz;
		const idVec3 edge1 = mesh[indexes[i + 1]].xyz - a;
		const idVec3 edge2 = mesh[indexes[i + 2]].xyz - a;
		const idVec3 p = dir.Cross( edge2 );
		const float det = edge1 * p;
		if ( idMath::Fabs( det ) < RAY_PARALLEL ) {
			continue;
		}
		const idVec3 toStart = start - a;
		const float u = ( toStart * p ) / det;
		const idVec3 q = toStart.Cross( edge1 );
		const float v = ( dir * q ) / det;
		const float at = ( edge2 * q ) / det;
		if ( u >= 0.0f && v >= 0.0f && u + v <= 1.0f && at > 0.0f && ( !hit || at < distance ) ) {
			distance = at;
			hit = true;
		}
	}
	return hit;
}

idMapBrush * editorBrush::MakeBox( const idBounds & bounds, const char * material, float repeat, const idVec3 & origin ) {
	const idVec3 size = bounds[1] - bounds[0];
	// the portal of a doorway is the face across its thinnest axis
	int portalSide = -1;
	if ( ( declManager->FindMaterial( material )->GetContentFlags() & CONTENTS_AREAPORTAL ) != 0 ) {
		const int thin = ( size[0] <= size[1] && size[0] <= size[2] ) ? 0 : ( ( size[1] <= size[2] ) ? 1 : 2 );
		portalSide = thin * 2;
	}

	idMapBrush * const brush = new ( TAG_IDLIB ) idMapBrush();
	const idVec3 texMat[2] = { idVec3( 1.0f / repeat, 0.0f, 0.0f ), idVec3( 0.0f, 1.0f / repeat, 0.0f ) };
	for ( int i = 0; i < 6; i++ ) {
		const int axis = i / 2;
		idPlane plane;
		plane.Zero();
		if ( ( i & 1 ) == 0 ) {
			plane[axis] = 1.0f;
			plane[3] = -( bounds[1][axis] - origin[axis] );
		} else {
			plane[axis] = -1.0f;
			plane[3] = bounds[0][axis] - origin[axis];
		}
		idMapBrushSide * const side = new ( TAG_IDLIB ) idMapBrushSide();
		side->SetPlane( plane );
		side->SetTextureMatrix( texMat );
		side->SetOrigin( origin );
		side->SetMaterial( ( portalSide == -1 || portalSide == i ) ? material : NODRAW_MATERIAL );
		brush->AddSide( side );
	}
	return brush;
}

void editorBrush::Translate( idMapBrush & brush, const idVec3 & delta ) {
	for ( int i = 0; i < brush.GetNumSides(); i++ ) {
		idPlane plane = brush.GetSide( i )->GetPlane();
		plane[3] -= plane.Normal() * delta;
		brush.GetSide( i )->SetPlane( plane );
	}
}

namespace {

constexpr float		EXACT_EPSILON = 1.0e-6f;		// nearer than this to 0, 1 or -1 is meant to be it

[[nodiscard]] float Exact( float value ) {
	if ( idMath::Fabs( value ) < EXACT_EPSILON ) {
		return 0.0f;
	}
	if ( idMath::Fabs( idMath::Fabs( value ) - 1.0f ) < EXACT_EPSILON ) {
		return ( value > 0.0f ) ? 1.0f : -1.0f;
	}
	return value;
}

[[nodiscard]] idVec3 Exact( const idVec3 & v ) {
	return idVec3( Exact( v.x ), Exact( v.y ), Exact( v.z ) );
}

[[nodiscard]] double Dot( const idVec3 & a, const idVec3 & b ) {
	return static_cast<double>( a.x ) * b.x + static_cast<double>( a.y ) * b.y + static_cast<double>( a.z ) * b.z;
}

// the two axes a side's texture matrix is in, as the map format defines them for a normal
void TextureBase( const idVec3 & normal, idVec3 & texS, idVec3 & texT ) {
	idMapBrushSide side;
	idPlane plane;
	plane.SetNormal( normal );
	plane[3] = 0.0f;
	side.SetPlane( plane );
	const idVec3 identity[2] = { idVec3( 1.0f, 0.0f, 0.0f ), idVec3( 0.0f, 1.0f, 0.0f ) };
	side.SetTextureMatrix( identity );
	side.SetOrigin( vec3_origin );
	idVec4 v[2];
	side.GetTextureVectors( v );
	texS = Exact( v[0].ToVec3() );
	texT = Exact( v[1].ToVec3() );
}

}	// namespace

idMat3 editorBrush::AxisRotation( int axis, float degrees ) {
	float s, c;
	const float quarters = degrees / 90.0f;
	if ( quarters == idMath::Rint( quarters ) ) {
		// no sine of a float is exactly 0
		const float table[4][2] = { { 0.0f, 1.0f }, { 1.0f, 0.0f }, { 0.0f, -1.0f }, { -1.0f, 0.0f } };
		const int quarter = ( ( static_cast<int>( quarters ) % 4 ) + 4 ) % 4;
		s = table[quarter][0];
		c = table[quarter][1];
	} else {
		idMath::SinCos( DEG2RAD( degrees ), s, c );
	}
	const int u = ( axis + 1 ) % 3;
	const int v = ( axis + 2 ) % 3;
	idMat3 rotation = mat3_identity;
	// right-handed: u turns toward v
	rotation[u][u] = c;
	rotation[u][v] = s;
	rotation[v][u] = -s;
	rotation[v][v] = c;
	return rotation;
}

void editorBrush::Rotate( idMapBrush & brush, const idMat3 & rotation, const idVec3 & pivot, const idVec3 & origin ) {
	const idVec3 worldPivot = pivot + origin;
	for ( int i = 0; i < brush.GetNumSides(); i++ ) {
		idMapBrushSide & side = *brush.GetSide( i );
		const idPlane old = side.GetPlane();
		idVec3 normal = Exact( old.Normal() * rotation );
		normal.Normalize();
		normal = Exact( normal );

		// the plane keeps its distance from the pivot
		idPlane plane;
		plane.SetNormal( normal );
		plane[3] = static_cast<float>( static_cast<double>( old[3] ) + Dot( old.Normal(), pivot ) - Dot( normal, pivot ) ) + 0.0f;

		// The texture vectors in space turn like the brush; written in the new side's two axes they
		// are the old numbers times a 2x2 matrix, which for a quarter turn has only 0, 1 and -1.
		idVec3 oldS, oldT, newS, newT;
		TextureBase( old.Normal(), oldS, oldT );
		TextureBase( normal, newS, newT );
		const idVec3 turnedS = oldS * rotation;
		const idVec3 turnedT = oldT * rotation;
		const float base[2][2] = { { Exact( turnedS * newS ), Exact( turnedS * newT ) }, { Exact( turnedT * newS ), Exact( turnedT * newT ) } };
		idVec3 texMat[2];
		side.GetTextureMatrix( texMat[0], texMat[1] );
		idVec3 turned[2];
		for ( int j = 0; j < 2; j++ ) {
			const idVec3 before = oldS * texMat[j][0] + oldT * texMat[j][1];
			const idVec3 after = before * rotation;
			// + 0: a product with a zero can be -0, which the map file would show
			turned[j][0] = texMat[j][0] * base[0][0] + texMat[j][1] * base[1][0] + 0.0f;
			turned[j][1] = texMat[j][0] * base[0][1] + texMat[j][1] * base[1][1] + 0.0f;
			// the point at the pivot keeps its texture coordinate
			turned[j][2] = static_cast<float>( static_cast<double>( texMat[j][2] ) + Dot( worldPivot, before ) - Dot( worldPivot, after ) ) + 0.0f;
		}
		side.SetPlane( plane );
		side.SetTextureMatrix( turned );
	}
}

void editorBrush::TranslatePatch( idMapPatch & patch, const idVec3 & delta ) {
	for ( int i = 0; i < patch.GetNumVertices(); i++ ) {
		patch[i].xyz += delta;
	}
}

void editorBrush::RotatePatch( idMapPatch & patch, const idMat3 & rotation, const idVec3 & pivot ) {
	for ( int i = 0; i < patch.GetNumVertices(); i++ ) {
		patch[i].xyz = pivot + ( patch[i].xyz - pivot ) * rotation + vec3_origin;
	}
}

bool editorBrush::MoveFace( idMapBrush & brush, int side, float distance ) {
	editorBrushWindings_t before;
	if ( side < 0 || side >= brush.GetNumSides() || !Windings( brush, before ) || before[side].GetNumPoints() < 3 ) {
		return false;
	}
	const idPlane old = brush.GetSide( side )->GetPlane();
	idPlane plane = old;
	plane[3] -= distance;
	brush.GetSide( side )->SetPlane( plane );

	editorBrushWindings_t after;
	if ( !Windings( brush, after ) || NumFaces( after ) != NumFaces( before ) ) {
		brush.GetSide( side )->SetPlane( old );
		return false;
	}
	return true;
}

bool editorBrush::Hollow( const idMapBrush & brush, float thickness, idList<idMapBrush *> & walls ) {
	editorBrushWindings_t windings;
	if ( thickness <= 0.0f || !Windings( brush, windings ) ) {
		return false;
	}
	for ( int i = 0; i < brush.GetNumSides(); i++ ) {
		if ( windings[i].GetNumPoints() < 3 ) {
			continue;
		}
		// the brush, cut off behind the face: the face's plane, moved in and turned around
		idMapBrush * const wall = brush.Clone();
		const idMapBrushSide & face = *brush.GetSide( i );
		idMapBrushSide * const inner = new ( TAG_IDLIB ) idMapBrushSide( face );
		idPlane plane = face.GetPlane();
		plane[3] += thickness;
		inner->SetPlane( -plane );
		wall->AddSide( inner );

		editorBrushWindings_t wallWindings;
		const bool valid = Windings( *wall, wallWindings ) && wallWindings[wall->GetNumSides() - 1].GetNumPoints() >= 3;
		// without the sides the new one cut away, the opposite one of a box
		idMapBrush * const trimmed = new ( TAG_IDLIB ) idMapBrush();
		trimmed->epairs = wall->epairs;
		for ( int j = 0; valid && j < wall->GetNumSides(); j++ ) {
			if ( wallWindings[j].GetNumPoints() >= 3 ) {
				trimmed->AddSide( new ( TAG_IDLIB ) idMapBrushSide( *wall->GetSide( j ) ) );
			}
		}
		delete wall;
		walls.Append( trimmed );
		if ( !valid ) {
			walls.DeleteContents( true );
			return false;
		}
	}
	// the walls of two opposite faces must leave room between them
	idMapBrush * const room = brush.Clone();
	for ( int i = 0; i < room->GetNumSides(); i++ ) {
		idPlane plane = room->GetSide( i )->GetPlane();
		plane[3] += thickness;
		room->GetSide( i )->SetPlane( plane );
	}
	editorBrushWindings_t roomWindings;
	const bool hasRoom = Windings( *room, roomWindings );
	delete room;
	if ( !hasRoom ) {
		walls.DeleteContents( true );
		return false;
	}
	return true;
}

void editorBrush::Rebase( idMapBrush & brush, const idVec3 & oldOrigin, const idVec3 & newOrigin ) {
	for ( int i = 0; i < brush.GetNumSides(); i++ ) {
		idMapBrushSide & side = *brush.GetSide( i );
		idPlane plane = side.GetPlane();
		plane[3] += plane.Normal() * ( newOrigin - oldOrigin );
		side.SetPlane( plane );
		side.SetOrigin( newOrigin );
	}
}

void editorBrush::SetTexture( idMapBrushSide & side, float shiftS, float shiftT, float repeatS, float repeatT, float rotate ) {
	float s, c;
	idMath::SinCos( DEG2RAD( rotate ), s, c );
	const idVec3 texMat[2] = { idVec3( c / repeatS, -s / repeatS, shiftS ), idVec3( s / repeatT, c / repeatT, shiftT ) };
	side.SetTextureMatrix( texMat );
}

void editorBrush::GetTexture( const idMapBrushSide & side, float & shiftS, float & shiftT, float & repeatS, float & repeatT, float & rotate ) {
	idVec3 texMat[2];
	side.GetTextureMatrix( texMat[0], texMat[1] );
	const float lengthS = texMat[0].ToVec2().Length();
	const float lengthT = texMat[1].ToVec2().Length();
	shiftS = texMat[0][2];
	shiftT = texMat[1][2];
	rotate = RAD2DEG( idMath::ATan( -texMat[0][1], texMat[0][0] ) );
	repeatS = ( lengthS > 0.0f ) ? 1.0f / lengthS : 0.0f;
	repeatT = ( lengthT > 0.0f ) ? 1.0f / lengthT : 0.0f;
	// a mirrored texture: T runs against the rotated axis
	if ( texMat[0][0] * texMat[1][1] - texMat[0][1] * texMat[1][0] < 0.0f ) {
		repeatT = -repeatT;
	}
}
