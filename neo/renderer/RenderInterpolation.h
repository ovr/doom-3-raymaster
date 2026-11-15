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

#ifndef __RENDERINTERPOLATION_H__
#define __RENDERINTERPOLATION_H__

/*
===============================================================================

	Render interpolation math (RenderWorld_interp.cpp), header only so idlibTests can test it.

	A def that moved in the last game tic is drawn between its pose after the tic before (prev)
	and its tic pose (cur), alpha = how far the frame is from the last tic towards the next one:

		origin = prev.origin + alpha * ( cur.origin - prev.origin )
		axis   = slerp( prev.axis, cur.axis, alpha )	both axes are rotations
		         row lerp								else ( g_gunScale scales the view weapon's axis )

	A pose that jumped is drawn at the tic pose: a move farther than r_interpolateSnapDist (a teleport)
	or a turn of more than 90 degrees, which a lerp would smear across the screen.

	A light lerps its origin and axis the same way, and only when they are all that changed: a new
	radius, frustum, shader or type would morph the light volume between two shapes.

===============================================================================
*/

// orthonormal rows, right handed; false for NaN
ID_INLINE bool R_IsRotationAxis( const idMat3 & axis ) {
	constexpr float epsilon = 1e-3f;
	for ( int i = 0; i < 3; i++ ) {
		if ( !( idMath::Fabs( axis[i].LengthSqr() - 1.0f ) <= epsilon ) ) {
			return false;
		}
		if ( !( idMath::Fabs( axis[i] * axis[( i + 1 ) % 3] ) <= epsilon ) ) {
			return false;
		}
	}
	return axis.Determinant() > 0.0f;
}

ID_INLINE idMat3 R_LerpAxis( const idMat3 & from, const idMat3 & to, const float t ) {
	if ( R_IsRotationAxis( from ) && R_IsRotationAxis( to ) ) {
		idQuat q;
		q.Slerp( from.ToQuat(), to.ToQuat(), t );
		// Slerp falls back to a plain lerp for nearly equal rotations
		q.Normalize();
		return q.ToMat3();
	}
	return from + ( to - from ) * t;
}

// cosine of the rotation between two axes; the rows are normalized, so a scaled axis compares by orientation
ID_INLINE float R_AxisRotationCos( const idMat3 & a, const idMat3 & b ) {
	float trace = 0.0f;
	for ( int i = 0; i < 3; i++ ) {
		idVec3 ra = a[i];
		idVec3 rb = b[i];
		ra.Normalize();
		rb.Normalize();
		trace += ra * rb;
	}
	return 0.5f * ( trace - 1.0f );
}

// too far apart to interpolate: a move farther than snapDist (0 = no limit), a turn of more than 90 degrees, NaN
ID_INLINE bool R_PoseJumped( const idVec3 & fromOrigin, const idMat3 & fromAxis, const idVec3 & toOrigin, const idMat3 & toAxis, const float snapDist ) {
	const float distSqr = ( toOrigin - fromOrigin ).LengthSqr();
	if ( !( distSqr < idMath::INFINITY ) ) {
		return true;
	}
	if ( snapDist > 0.0f && distSqr > snapDist * snapDist ) {
		return true;
	}
	return !( R_AxisRotationCos( fromAxis, toAxis ) >= 0.0f );
}

// the shape of a light is the same in two updates, so only its pose (and per-frame parms) changed;
// prelightDropped: UpdateLightDef has dropped the prelight of a light that moved, the game still passes it
ID_INLINE bool R_LightShapeMatches( const renderLight_t & from, const renderLight_t & to, const bool prelightDropped ) {
	return from.lightRadius == to.lightRadius && from.lightCenter == to.lightCenter
		&& from.target == to.target && from.right == to.right && from.up == to.up
		&& from.start == to.start && from.end == to.end
		&& from.pointLight == to.pointLight && from.parallel == to.parallel
		&& from.shader == to.shader
		&& ( prelightDropped || from.prelightModel == to.prelightModel );
}

#endif /* !__RENDERINTERPOLATION_H__ */
