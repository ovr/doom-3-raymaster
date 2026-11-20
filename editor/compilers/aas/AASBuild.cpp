/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company. 
Copyright (C) 2026 Dmitry Patsura

This file is part of the Doom 3 GPL Source Code (?Doom 3 Source Code?).  

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

#pragma hdrstop
#include "idlib/precompiled.h"

#include "AASBuild_local.h"

#define BFL_PATCH		0x1000

//===============================================================
//
//	idAASBuild
//
//===============================================================

/*
============
idAASBuild::idAASBuild
============
*/
idAASBuild::idAASBuild() {
	file = nullptr;
	procNodes = nullptr;
	numProcNodes = 0;
	numGravitationalSubdivisions = 0;
	numMergedLeafNodes = 0;
	numLedgeSubdivisions = 0;
	ledgeMap = nullptr;
}

/*
============
idAASBuild::~idAASBuild
============
*/
idAASBuild::~idAASBuild() {
	Shutdown();
}

/*
================
idAASBuild::Shutdown
================
*/
void idAASBuild::Shutdown() {
	aasSettings = nullptr;
	if ( file ) {
		delete file;
		file = nullptr;
	}
	DeleteProcBSP();
	ShutdownHash();
	numGravitationalSubdivisions = 0;
	numMergedLeafNodes = 0;
	numLedgeSubdivisions = 0;
	ledgeList.Clear();
	if ( ledgeMap ) {
		delete ledgeMap;
		ledgeMap = nullptr;
	}
}

/*
================
idAASBuild::ParseProcNodes
================
*/
void idAASBuild::ParseProcNodes( idLexer *src ) {
	int i;

	src->ExpectTokenString( "{" );

	idAASBuild::numProcNodes = src->ParseInt();
	if ( idAASBuild::numProcNodes < 0 ) {
		src->Error( "idAASBuild::ParseProcNodes: bad numProcNodes" );
	}
	idAASBuild::procNodes = (aasProcNode_t *)Mem_ClearedAlloc( idAASBuild::numProcNodes * sizeof( aasProcNode_t ), TAG_AAS );

	for ( i = 0; i < idAASBuild::numProcNodes; i++ ) {
		aasProcNode_t *node;

		node = &(idAASBuild::procNodes[i]);

		src->Parse1DMatrix( 4, node->plane.ToFloatPtr() );
		node->children[0] = src->ParseInt();
		node->children[1] = src->ParseInt();
	}

	src->ExpectTokenString( "}" );
}

/*
================
idAASBuild::LoadProcBSP
================
*/
bool idAASBuild::LoadProcBSP( const char *name, ID_TIME_T minFileTime ) {
	idStr fileName;
	idToken token;
	idLexer *src;

	// load it
	fileName = name;
	fileName.SetFileExtension( PROC_FILE_EXT );
	src = new idLexer( fileName, LEXFL_NOSTRINGCONCAT | LEXFL_NODOLLARPRECOMPILE );
	if ( !src->IsLoaded() ) {
		common->Warning("idAASBuild::LoadProcBSP: couldn't load %s", fileName.c_str() );
		delete src;
		return false;
	}

	// if the file is too old
	if ( src->GetFileTime() < minFileTime ) {
		delete src;
		return false;
	}

	if ( !src->ReadToken( &token ) || token.Icmp( PROC_FILE_ID ) ) {
		common->Warning( "idAASBuild::LoadProcBSP: bad id '%s' instead of '%s'", token.c_str(), PROC_FILE_ID );
		delete src;
		return false;
	}

	// parse the file
	while ( 1 ) {
		if ( !src->ReadToken( &token ) ) {
			break;
		}

		if ( token == "model" ) {
			src->SkipBracedSection();
			continue;
		}

		if ( token == "shadowModel" ) {
			src->SkipBracedSection();
			continue;
		}

		if ( token == "interAreaPortals" ) {
			src->SkipBracedSection();
			continue;
		}

		if ( token == "nodes" ) {
			idAASBuild::ParseProcNodes( src );
			break;
		}

		src->Error( "idAASBuild::LoadProcBSP: bad token \"%s\"", token.c_str() );
	}

	delete src;

	return true;
}

/*
============
idAASBuild::DeleteProcBSP
============
*/
void idAASBuild::DeleteProcBSP() {
	if ( procNodes ) {
		Mem_Free( procNodes );
		procNodes = nullptr;
	}
	numProcNodes = 0;
}

/*
============
idAASBuild::ChoppedAwayByProcBSP
============
*/
bool idAASBuild::ChoppedAwayByProcBSP( int nodeNum, idFixedWinding *w, const idVec3 &normal, const idVec3 &origin, const float radius ) {
	int res;
	idFixedWinding back;
	aasProcNode_t *node;
	float dist;

	do {
		node = idAASBuild::procNodes + nodeNum;
		dist = node->plane.Normal() * origin + node->plane[3];
		if ( dist > radius ) {
			res = SIDE_FRONT;
		}
		else if ( dist < -radius ) {
			res = SIDE_BACK;
		}
		else {
			res = w->Split( &back, node->plane, ON_EPSILON );
		}
		if ( res == SIDE_FRONT ) {
			nodeNum = node->children[0];
		}
		else if ( res == SIDE_BACK ) {
			nodeNum = node->children[1];
		}
		else if ( res == SIDE_ON ) {
			// continue with the side the winding faces
			if ( node->plane.Normal() * normal > 0.0f ) {
				nodeNum = node->children[0];
			}
			else {
				nodeNum = node->children[1];
			}
		}
		else {
			// if either node is not solid
			if ( node->children[0] < 0 || node->children[1] < 0 ) {
				return false;
			}
			// only recurse if the node is not solid
			if ( node->children[1] > 0 ) {
				if ( !idAASBuild::ChoppedAwayByProcBSP( node->children[1], &back, normal, origin, radius ) ) {
					return false;
				}
			}
			nodeNum = node->children[0];
		}
	} while ( nodeNum > 0 );
	if ( nodeNum < 0 ) {
		return false;
	}
	return true;
}

/*
============
idAASBuild::ClipBrushSidesWithProcBSP
============
*/
void idAASBuild::ClipBrushSidesWithProcBSP( idBrushList &brushList ) {
	int i, clippedSides;
	idBrush *brush;
	idFixedWinding neww;
	idBounds bounds;
	float radius;
	idVec3 origin;

	// if the .proc file has no BSP tree
	if ( idAASBuild::procNodes == nullptr ) {
		return;
	}

	clippedSides = 0;
	for ( brush = brushList.Head(); brush; brush = brush->Next() ) {
		for ( i = 0; i < brush->GetNumSides(); i++ ) {

			if ( !brush->GetSide(i)->GetWinding() ) {
				continue;
			}

			// make a local copy of the winding
			neww = *brush->GetSide(i)->GetWinding();
			neww.GetBounds( bounds );
			origin = (bounds[1] - bounds[0]) * 0.5f;
			radius = origin.Length() + ON_EPSILON;
			origin = bounds[0] + origin;

			if ( ChoppedAwayByProcBSP( 0, &neww, brush->GetSide(i)->GetPlane().Normal(), origin, radius ) ) {
				brush->GetSide(i)->SetFlag( SFL_USED_SPLITTER );
				clippedSides++;
			}
		}
	}

	common->Printf( "%6d brush sides clipped\n", clippedSides );
}

/*
============
idAASBuild::ContentsForAAS
============
*/
int idAASBuild::ContentsForAAS( int contents ) {
	int c;

	if ( contents & ( CONTENTS_SOLID|CONTENTS_AAS_SOLID|CONTENTS_MONSTERCLIP ) ) {
		return AREACONTENTS_SOLID;
	}
	c = 0;
	if ( contents & CONTENTS_WATER ) {
		c |= AREACONTENTS_WATER;
	}
	if ( contents & CONTENTS_AREAPORTAL ) {
		c |= AREACONTENTS_CLUSTERPORTAL;
	}
	if ( contents & CONTENTS_AAS_OBSTACLE ) {
		c |= AREACONTENTS_OBSTACLE;
	}
	return c;
}

/*
============
idAASBuild::AddBrushForMapBrush
============
*/
idBrushList idAASBuild::AddBrushesForMapBrush( const idMapBrush *mapBrush, const idVec3 &origin, const idMat3 &axis, int entityNum, int primitiveNum, idBrushList brushList ) {
	int contents, i;
	idMapBrushSide *mapSide;
	const idMaterial *mat;
	idList<idBrushSide *> sideList;
	idBrush *brush;
	idPlane plane;

	contents = 0;
	for ( i = 0; i < mapBrush->GetNumSides(); i++ ) {
		mapSide = mapBrush->GetSide(i);
		mat = declManager->FindMaterial( mapSide->GetMaterial() );
		contents |= mat->GetContentFlags();
		plane = mapSide->GetPlane();
		plane.FixDegeneracies( DEGENERATE_DIST_EPSILON );
		sideList.Append( new idBrushSide( plane, -1 ) );
	}

	contents = ContentsForAAS( contents );
	if ( !contents ) {
		for ( i = 0; i < sideList.Num(); i++ ) {
			delete sideList[i];
		}
		return brushList;
	}

	brush = new idBrush();
	brush->SetContents( contents );

	if ( !brush->FromSides( sideList ) ) {
		common->Warning( "brush primitive %d on entity %d is degenerate", primitiveNum, entityNum );
		delete brush;
		return brushList;
	}

	brush->SetEntityNum( entityNum );
	brush->SetPrimitiveNum( primitiveNum );
	brush->Transform( origin, axis );
	brushList.AddToTail( brush );

	return brushList;
}

/*
============
idAASBuild::AddBrushesForPatch
============
*/
idBrushList idAASBuild::AddBrushesForMapPatch( const idMapPatch *mapPatch, const idVec3 &origin, const idMat3 &axis, int entityNum, int primitiveNum, idBrushList brushList ) {
	int i, j, contents, validBrushes;
	float dot;
	int v1, v2, v3, v4;
	idFixedWinding w;
	idPlane plane;
	idVec3 d1, d2;
	idBrush *brush;
	idSurface_Patch mesh;
	const idMaterial *mat;

	mat = declManager->FindMaterial( mapPatch->GetMaterial() );
	contents = ContentsForAAS( mat->GetContentFlags() );

	if ( !contents ) {
		return brushList;
	}

	mesh = idSurface_Patch( *mapPatch );

	// if the patch has an explicit number of subdivisions use it to avoid cracks
	if ( mapPatch->GetExplicitlySubdivided() ) {
		mesh.SubdivideExplicit( mapPatch->GetHorzSubdivisions(), mapPatch->GetVertSubdivisions(), false, true );
	} else {
		mesh.Subdivide( DEFAULT_CURVE_MAX_ERROR_CD, DEFAULT_CURVE_MAX_ERROR_CD, DEFAULT_CURVE_MAX_LENGTH_CD, false );
	}

	validBrushes = 0;

	for ( i = 0; i < mesh.GetWidth() - 1; i++ ) {
		for ( j = 0; j < mesh.GetHeight() - 1; j++ ) {

			v1 = j * mesh.GetWidth() + i;
			v2 = v1 + 1;
			v3 = v1 + mesh.GetWidth() + 1;
			v4 = v1 + mesh.GetWidth();

			d1 = mesh[v2].xyz - mesh[v1].xyz;
			d2 = mesh[v3].xyz - mesh[v1].xyz;
			plane.SetNormal( d1.Cross(d2) );
			if ( plane.Normalize() != 0.0f ) {
				plane.FitThroughPoint( mesh[v1].xyz );
				dot = plane.Distance( mesh[v4].xyz );
				// if we can turn it into a quad
				if ( idMath::Fabs(dot) < 0.1f ) {
					w.Clear();
					w += mesh[v1].xyz;
					w += mesh[v2].xyz;
					w += mesh[v3].xyz;
					w += mesh[v4].xyz;

					brush = new idBrush();
					brush->SetContents( contents );
					if ( brush->FromWinding( w, plane ) ) {
						brush->SetEntityNum( entityNum );
						brush->SetPrimitiveNum( primitiveNum );
						brush->SetFlag( BFL_PATCH );
						brush->Transform( origin, axis );
						brushList.AddToTail( brush );
						validBrushes++;
					}
					else {
						delete brush;
					}
					continue;
				}
				else {
					// create one of the triangles
					w.Clear();
					w += mesh[v1].xyz;
					w += mesh[v2].xyz;
					w += mesh[v3].xyz;

					brush = new idBrush();
					brush->SetContents( contents );
					if ( brush->FromWinding( w, plane ) ) {
						brush->SetEntityNum( entityNum );
						brush->SetPrimitiveNum( primitiveNum );
						brush->SetFlag( BFL_PATCH );
						brush->Transform( origin, axis );
						brushList.AddToTail( brush );
						validBrushes++;
					}
					else {
						delete brush;
					}
				}
			}
			// create the other triangle
			d1 = mesh[v3].xyz - mesh[v1].xyz;
			d2 = mesh[v4].xyz - mesh[v1].xyz;
			plane.SetNormal( d1.Cross(d2) );
			if ( plane.Normalize() != 0.0f ) {
				plane.FitThroughPoint( mesh[v1].xyz );

				w.Clear();
				w += mesh[v1].xyz;
				w += mesh[v3].xyz;
				w += mesh[v4].xyz;

				brush = new idBrush();
				brush->SetContents( contents );
				if ( brush->FromWinding( w, plane ) ) {
					brush->SetEntityNum( entityNum );
					brush->SetPrimitiveNum( primitiveNum );
					brush->SetFlag( BFL_PATCH );
					brush->Transform( origin, axis );
					brushList.AddToTail( brush );
					validBrushes++;
				}
				else {
					delete brush;
				}
			}
		}
	}

	if ( !validBrushes ) {
		common->Warning( "patch primitive %d on entity %d is completely degenerate", primitiveNum, entityNum );
	}

	return brushList;
}

/*
============
idAASBuild::AddBrushesForMapEntity
============
*/
idBrushList idAASBuild::AddBrushesForMapEntity( const idMapEntity *mapEnt, int entityNum, idBrushList brushList ) {
	int i;
	idVec3 origin;
	idMat3 axis;

	if ( mapEnt->GetNumPrimitives() < 1 ) {
		return brushList;
	}

	mapEnt->epairs.GetVector( "origin", "0 0 0", origin );
	if ( !mapEnt->epairs.GetMatrix( "rotation", "1 0 0 0 1 0 0 0 1", axis ) ) {
		float angle = mapEnt->epairs.GetFloat( "angle" );
		if ( angle != 0.0f ) {
			axis = idAngles( 0.0f, angle, 0.0f ).ToMat3();
		} else {
			axis.Identity();
		}
	}

	for ( i = 0; i < mapEnt->GetNumPrimitives(); i++ ) {
		idMapPrimitive	*mapPrim;

		mapPrim = mapEnt->GetPrimitive(i);
		if ( mapPrim->GetType() == idMapPrimitive::TYPE_BRUSH ) {
			brushList = AddBrushesForMapBrush( static_cast<idMapBrush*>(mapPrim), origin, axis, entityNum, i, brushList );
			continue;
		}
		if ( mapPrim->GetType() == idMapPrimitive::TYPE_PATCH ) {
			if ( aasSettings->usePatches ) {
				brushList = AddBrushesForMapPatch( static_cast<idMapPatch*>(mapPrim), origin, axis, entityNum, i, brushList );
			}
			continue;
		}
	}

	return brushList;
}

/*
============
idAASBuild::AddBrushesForMapFile
============
*/
idBrushList idAASBuild::AddBrushesForMapFile( const idMapFile * mapFile, idBrushList brushList ) {
	int i;

	common->Printf( "[Brush Load]\n" );

	brushList = AddBrushesForMapEntity( mapFile->GetEntity( 0 ), 0, brushList );

	for ( i = 1; i < mapFile->GetNumEntities(); i++ ) {
		const char *classname = mapFile->GetEntity( i )->epairs.GetString( "classname" );

		if ( idStr::Icmp( classname, "func_aas_obstacle" ) == 0 ) {
			brushList = AddBrushesForMapEntity( mapFile->GetEntity( i ), i, brushList );
		}
	}

	common->Printf( "%6d brushes\n", brushList.Num() );

	return brushList;
}

/*
============
idAASBuild::CheckForEntities
============
*/
bool idAASBuild::CheckForEntities( const idMapFile *mapFile, idStrList &entityClassNames ) const {
	int		i;
	idStr	classname;

	for ( i = 0; i < mapFile->GetNumEntities(); i++ ) {
		if ( !mapFile->GetEntity(i)->epairs.GetString( "classname", "", classname ) ) {
			continue;
		}

		if ( aasSettings->ValidEntity( classname ) ) {
			entityClassNames.AddUnique( classname );
		}
	}

	return ( entityClassNames.Num() != 0 );
}

/*
============
MergeAllowed
============
*/
bool MergeAllowed( idBrush *b1, idBrush *b2 ) {
	return ( b1->GetContents() == b2->GetContents() && !( ( b1->GetFlags() | b2->GetFlags() ) & BFL_PATCH ) );
}

/*
============
ExpandedChopAllowed
============
*/
bool ExpandedChopAllowed( idBrush *b1, idBrush *b2 ) {
	return ( b1->GetContents() == b2->GetContents() );
}

/*
============
ExpandedMergeAllowed
============
*/
bool ExpandedMergeAllowed( idBrush *b1, idBrush *b2 ) {
	return ( b1->GetContents() == b2->GetContents() );
}

/*
============
idAASBuild::ChangeMultipleBoundingBoxContents
============
*/
void idAASBuild::ChangeMultipleBoundingBoxContents_r( idBrushBSPNode *node, int mask ) {
	while( node ) {
		if ( !( node->GetContents() & mask ) ) {
			node->SetContents( node->GetContents() & ~AREACONTENTS_SOLID );
		}
		ChangeMultipleBoundingBoxContents_r( node->GetChild( 0 ), mask );
		node = node->GetChild( 1 );
	}
}

/*
============
idAASBuild::Build

No engine error for what a mapper can cause: the answer says what happened.
============
*/
aasBuildResult_t idAASBuild::Build( const idStr &fileName, const idAASSettings *settings, const aasStageCallback_t &stage, idStr &error ) {
	idBrushList brushList;
	idBrushBSP bsp;
	idAASReach reach;
	idAASCluster cluster;
	idStrList entityClassNames;

	const int startTime = Sys_Milliseconds();

	Shutdown();

	aasSettings = settings;

	idStr name = fileName;
	name.SetFileExtension( "map" );

	stage( "load" );
	const std::unique_ptr<idMapFile> mapFile = std::make_unique<idMapFile>();
	if ( !mapFile->Parse( name ) ) {
		sprintf( error, "Couldn't load map file: '%s'", name.c_str() );
		return aasBuildResult_t::FAILED;
	}

	// check if this map has any entities that use this AAS file
	if ( !CheckForEntities( mapFile.get(), entityClassNames ) ) {
		common->Printf( "no entities in map that use %s\n", settings->fileExtension.c_str() );
		return aasBuildResult_t::NOT_USED;
	}

	// load map file brushes
	brushList = AddBrushesForMapFile( mapFile.get(), brushList );

	// if empty map
	if ( brushList.Num() == 0 ) {
		sprintf( error, "%s is empty", name.c_str() );
		return aasBuildResult_t::FAILED;
	}

	// merge as many brushes as possible before expansion
	stage( "merge brushes" );
	brushList.Merge( MergeAllowed );

	// if there is a .proc file newer than the .map file
	if ( LoadProcBSP( fileName, mapFile->GetFileTime() ) ) {
		ClipBrushSidesWithProcBSP( brushList );
		DeleteProcBSP();
	}

	// make copies of the brush list and expand the brushes for the axial bounding boxes
	stage( "expand brushes" );
	int mask = AREACONTENTS_SOLID;
	idBrushList allBrushes;
	for ( int i = 0; i < aasSettings->numBoundingBoxes; i++ ) {
		const bool last = ( i == aasSettings->numBoundingBoxes - 1 );
		const std::unique_ptr<idBrushList> expanded( last ? nullptr : brushList.Copy() );
		idBrushList &list = last ? brushList : *expanded;
		const int bit = 1 << ( i + AREACONTENTS_BBOX_BIT );
		mask |= bit;
		for ( idBrush *b = list.Head(); b; b = b->Next() ) {
			b->ExpandForAxialBox( aasSettings->boundingBoxes[i] );
			b->SetContents( b->GetContents() | bit );
		}
		allBrushes.AddToTail( list );
	}

	if ( aasSettings->writeBrushMap ) {
		bsp.WriteBrushMap( fileName, "_" + aasSettings->fileExtension, AREACONTENTS_SOLID );
	}

	// build BSP tree from brushes
	stage( "bsp" );
	bsp.Build( allBrushes, AREACONTENTS_SOLID, ExpandedChopAllowed, ExpandedMergeAllowed );

	// only solid nodes with all bits set for all bounding boxes need to stay solid
	ChangeMultipleBoundingBoxContents_r( bsp.GetRootNode(), mask );

	// portalize the bsp tree
	stage( "portals" );
	bsp.Portalize();

	// remove subspaces not reachable by entities
	if ( !bsp.RemoveOutside( mapFile.get(), AREACONTENTS_SOLID, entityClassNames ) ) {
		bsp.LeakFile( name );
		common->Printf( "%s has no outside\n", name.c_str() );
		return aasBuildResult_t::LEAKED;
	}

	// gravitational subdivision
	stage( "gravitational subdivision" );
	GravitationalSubdivision( bsp );

	// merge portals where possible
	bsp.MergePortals( AREACONTENTS_SOLID );

	// melt portal windings
	bsp.MeltPortals( AREACONTENTS_SOLID );

	if ( aasSettings->writeBrushMap ) {
		WriteLedgeMap( fileName, "_" + aasSettings->fileExtension + "_ledge" );
	}

	// ledge subdivisions
	stage( "ledge subdivision" );
	LedgeSubdivision( bsp );

	// merge leaf nodes
	stage( "merge leaf nodes" );
	MergeLeafNodes( bsp );

	// merge portals where possible
	bsp.MergePortals( AREACONTENTS_SOLID );

	// melt portal windings
	bsp.MeltPortals( AREACONTENTS_SOLID );

	// store the file from the bsp tree
	stage( "store" );
	StoreFile( bsp );
	file->settings = *aasSettings;

	// calculate reachability
	stage( "reachability" );
	reach.Build( mapFile.get(), file );

	// build clusters
	stage( "clusters" );
	cluster.Build( file );

	// optimize the file
	if ( !aasSettings->noOptimize ) {
		file->Optimize();
	}

	// write the file
	name.SetFileExtension( aasSettings->fileExtension );
	if ( !file->Write( name, mapFile->GetGeometryCRC() ) ) {
		sprintf( error, "Couldn't write '%s'", name.c_str() );
		return aasBuildResult_t::FAILED;
	}

	common->Printf( "%6d seconds to create AAS\n", (Sys_Milliseconds() - startTime) / 1000 );

	return aasBuildResult_t::WRITTEN;
}

/*
============
idAASBuild::BuildReachability

The reachabilities and clusters of an AAS file that exists, made again.
============
*/
aasBuildResult_t idAASBuild::BuildReachability( const idStr &fileName, const idAASSettings *settings, const aasStageCallback_t &stage, idStr &error ) {
	idAASReach reach;
	idAASCluster cluster;

	const int startTime = Sys_Milliseconds();

	Shutdown();

	aasSettings = settings;

	idStr name = fileName;
	name.SetFileExtension( "map" );

	stage( "load" );
	const std::unique_ptr<idMapFile> mapFile = std::make_unique<idMapFile>();
	if ( !mapFile->Parse( name ) ) {
		sprintf( error, "Couldn't load map file: '%s'", name.c_str() );
		return aasBuildResult_t::FAILED;
	}

	file = new ( TAG_AAS ) idAASFileLocal();

	name.SetFileExtension( aasSettings->fileExtension );
	if ( fileSystem->GetFileLength( name ) <= 0 ) {
		return aasBuildResult_t::NOT_USED;
	}
	if ( !file->Load( name, 0 ) ) {
		sprintf( error, "Couldn't load AAS file: '%s'", name.c_str() );
		return aasBuildResult_t::FAILED;
	}

	file->settings = *aasSettings;

	// calculate reachability
	stage( "reachability" );
	reach.Build( mapFile.get(), file );

	// build clusters
	stage( "clusters" );
	cluster.Build( file );

	// write the file
	if ( !file->Write( name, mapFile->GetGeometryCRC() ) ) {
		sprintf( error, "Couldn't write '%s'", name.c_str() );
		return aasBuildResult_t::FAILED;
	}

	common->Printf( "%6d seconds to calculate reachability\n", (Sys_Milliseconds() - startTime) / 1000 );

	return aasBuildResult_t::WRITTEN;
}

/*
============
AASCompile

Every AAS type of def/aas.def that an entity of the map uses. Leaves through an exception
on an engine error or a cancel; AASFree has to follow in any case.
============
*/
void AASCompile( const aasCompileOptions_t &options, aasCompileResult_t &result ) {
	AASSetCancelCheck( &options.cancelled );

	// A new map has no resource file of its own, its media are in the other maps'.
	fileSystem->MountAllMapResources();

	idStr mapName = options.mapName;
	mapName.BackSlashesToSlashes();
	if ( mapName.Icmpn( "maps/", 4 ) != 0 ) {
		mapName = "maps/" + mapName;
	}
	mapName.StripFileExtension();

	// only the one the compiler wrote, never in the game install
	const idStr leakFile = mapName + ".lin";

	// get the aas settings definitions
	const idDict *dict = gameEdit->FindEntityDefDict( "aas_types", false );
	if ( dict == nullptr ) {
		result.status = compileStatus_t::FAILED;
		result.error = "Unable to find entityDef for 'aas_types'";
		return;
	}

	idAASBuild aas;
	for ( const idKeyValue *kv = dict->MatchPrefix( "type" ); kv != nullptr; kv = dict->MatchPrefix( "type", kv ) ) {
		const idDict *settingsDict = gameEdit->FindEntityDefDict( kv->GetValue(), false );
		if ( settingsDict == nullptr ) {
			common->Warning( "Unable to find '%s' in def/aas.def", kv->GetValue().c_str() );
			continue;
		}
		common->Printf( "=======================================================\n" );

		idAASSettings settings;
		settings.FromDict( kv->GetValue(), settingsDict );
		settings.usePatches = settings.usePatches || options.usePatches;
		settings.writeBrushMap = settings.writeBrushMap || options.writeBrushMap;
		settings.playerFlood = settings.playerFlood || options.playerFlood;
		settings.noOptimize = settings.noOptimize || options.noOptimize;

		const idStr typeName = settings.fileExtension;
		const aasStageCallback_t stage = [&options, &typeName]( const char *name ) {
			if ( options.progress ) {
				options.progress( va( "%s: %s", typeName.c_str(), name ), 0.0f );
			}
			AASPoll();
		};

		idStr error;
		const aasBuildResult_t built = options.reachabilityOnly
			? aas.BuildReachability( mapName, &settings, stage, error )
			: aas.Build( mapName, &settings, stage, error );
		switch ( built ) {
			case aasBuildResult_t::WRITTEN: {
				aasFileResult_t &written = result.files.Alloc();
				written.fileName = mapName + "." + settings.fileExtension;
				aas.GetCounts( written );
				break;
			}
			case aasBuildResult_t::NOT_USED:
				break;
			case aasBuildResult_t::LEAKED:
				result.status = compileStatus_t::LEAKED;
				result.leakFile = leakFile;
				return;
			case aasBuildResult_t::FAILED:
				result.status = compileStatus_t::FAILED;
				result.error = error;
				return;
		}
		aas.Shutdown();
	}

	result.status = compileStatus_t::OK;
}

/*
============
AASFree

Ends a compile, whichever way it went. Returns the number of objects the compiler had not freed itself.
============
*/
int AASFree() {
	AASSetCancelCheck( nullptr );
	const int left = idAASCompileObject::NumLive();
	idAASCompileObject::FreeAll();
	return left;
}
