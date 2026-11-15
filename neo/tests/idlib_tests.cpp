/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

/*
===============================================================================

	idLib unit tests

	Standalone console program that links the idLib objects and checks the
	math used by the physics code (articulated figures and rigid bodies):
	scalar math, vectors and rotations, trace model mass properties,
	idMatX factorizations, the LCP solvers and the SIMD processors.

	Every check also verifies that results are finite, because a single NaN
	in a physics state makes every collision query test the whole world.

	Usage: idlibTests [filter [noftz]]
		filter		only run the tests whose name contains this, "all" runs everything
		noftz		don't enable flush-to-zero / denormals-are-zero like the game threads do

===============================================================================
*/

#pragma hdrstop
#include "../idlib/precompiled.h"

#include "../idlib/math/Lcp.h"

// the same solvers compiled without SSE (lcp_generic.cpp)
#undef __MATH_LCP_H__
#define idLCP idLCP_Generic
#include "../idlib/math/Lcp.h"
#undef idLCP
#include "../idlib/math/Simd_Generic.h"
#include "../idlib/math/Simd_SSE.h"

#include "../renderer/RenderInterpolation.h"

#include <stdio.h>
#include <math.h>
#include <float.h>
#include <limits>

// idSIMD::InitProcessor needs the engine (common, sys), so the tests create the processors themselves
static idSIMDProcessor *	genericProcessor = NULL;
static idSIMDProcessor *	sseProcessor = NULL;

/*
===============================================================================

	Minimal test framework

===============================================================================
*/

typedef void ( *testFunc_t )();

struct testCase_t {
	const char *	name;
	testFunc_t		func;
};

static const int	MAX_TESTS = 256;
static testCase_t	testCases[MAX_TESTS];
static int			numTestCases = 0;

static const char *	currentTest = "";
static int			numChecks = 0;
static int			numFailures = 0;
static int			numFailuresInTest = 0;
static int			MAX_FAILURES_PER_TEST = 20;		// don't flood the output when a loop fails

struct testRegistrar_t {
	testRegistrar_t( const char *name, testFunc_t func ) {
		if ( numTestCases < MAX_TESTS ) {
			testCases[numTestCases].name = name;
			testCases[numTestCases].func = func;
			numTestCases++;
		}
	}
};

#define TEST( name ) \
	static void Test_##name(); \
	static testRegistrar_t testRegistrar_##name( #name, Test_##name ); \
	static void Test_##name()

static void ReportFailure( const char *file, int line, const char *expr, const char *fmt, ... ) {
	numFailures++;
	numFailuresInTest++;
	if ( numFailuresInTest > MAX_FAILURES_PER_TEST ) {
		return;
	}
	const char *shortFile = strrchr( file, '\\' );
	shortFile = ( shortFile != NULL ) ? shortFile + 1 : file;
	printf( "  FAIL %s:%d: %s", shortFile, line, expr );
	if ( fmt != NULL && fmt[0] != '\0' ) {
		va_list argptr;
		va_start( argptr, fmt );
		printf( " -- " );
		vprintf( fmt, argptr );
		va_end( argptr );
	}
	printf( "\n" );
}

#define CHECK( cond, ... ) \
	do { \
		numChecks++; \
		if ( !( cond ) ) { \
			ReportFailure( __FILE__, __LINE__, #cond, "" __VA_ARGS__ ); \
		} \
	} while ( 0 )

// NaN fails every comparison, so this rejects NaN and infinity
static bool IsFinite( float f ) {
	return f > -1e30f && f < 1e30f;
}

static bool IsFinite( const float *p, int n ) {
	for ( int i = 0; i < n; i++ ) {
		if ( !IsFinite( p[i] ) ) {
			return false;
		}
	}
	return true;
}

static bool IsFinite( const idVec3 &v ) { return IsFinite( v.ToFloatPtr(), 3 ); }
static bool IsFinite( const idMat3 &m ) { return IsFinite( m.ToFloatPtr(), 9 ); }
static bool IsFinite( const idVecX &v ) { return IsFinite( v.ToFloatPtr(), v.GetSize() ); }
static bool IsFinite( const idMatX &m ) { return IsFinite( m.ToFloatPtr(), m.GetNumRows() * m.GetNumColumns() ); }

static bool Near( float a, float b, float epsilon ) {
	return idMath::Fabs( a - b ) <= epsilon * Max( 1.0f, Max( idMath::Fabs( a ), idMath::Fabs( b ) ) );
}

static bool IsOrthonormal( const idMat3 &m, float epsilon ) {
	const idMat3 p = m * m.Transpose();
	return p.Compare( mat3_identity, epsilon ) && Near( m.Determinant(), 1.0f, epsilon );
}

/*
===============================================================================

	Scalar math

===============================================================================
*/

TEST( Math_SqrtAndInvSqrt ) {
	CHECK( idMath::Sqrt( 0.0f ) == 0.0f, "Sqrt(0) = %g", idMath::Sqrt( 0.0f ) );
	CHECK( idMath::Sqrt( -1.0f ) == 0.0f, "Sqrt(-1) = %g", idMath::Sqrt( -1.0f ) );
	CHECK( Near( idMath::Sqrt( 4.0f ), 2.0f, 1e-6f ) );
	CHECK( IsFinite( idMath::Sqrt( 1e-40f ) ), "Sqrt(denormal) = %g", idMath::Sqrt( 1e-40f ) );
	CHECK( IsFinite( idMath::InvSqrt( 0.0f ) * 0.0f ), "InvSqrt(0) * 0 = %g", idMath::InvSqrt( 0.0f ) * 0.0f );
	CHECK( Near( idMath::InvSqrt( 4.0f ), 0.5f, 1e-6f ) );
	CHECK( Near( idMath::InvSqrt16( 4.0f ), 0.5f, 1e-3f ) );
	CHECK( Near( idMath::Sqrt16( 9.0f ), 3.0f, 1e-3f ) );

	for ( float x = 1e-30f; x < 1e30f; x *= 7.3f ) {
		CHECK( Near( idMath::Sqrt( x ), sqrtf( x ), 1e-5f ), "x = %g: %g vs %g", x, idMath::Sqrt( x ), sqrtf( x ) );
		CHECK( Near( idMath::InvSqrt( x ), 1.0f / sqrtf( x ), 1e-5f ), "x = %g", x );
	}
}

TEST( Math_TrigOutOfRange ) {
	// rounding errors produce values just outside [-1, 1], these must not become NaN
	const float values[] = { -1.0001f, -1.0f, -0.5f, 0.0f, 0.5f, 1.0f, 1.0001f, 1.00000012f, -1.00000012f };
	for ( int i = 0; i < sizeof( values ) / sizeof( values[0] ); i++ ) {
		const float v = values[i];
		CHECK( IsFinite( idMath::ACos( v ) ), "ACos(%.9g) = %g", v, idMath::ACos( v ) );
		CHECK( IsFinite( idMath::ASin( v ) ), "ASin(%.9g) = %g", v, idMath::ASin( v ) );
		CHECK( IsFinite( idMath::ACos16( v ) ), "ACos16(%.9g) = %g", v, idMath::ACos16( v ) );
		CHECK( IsFinite( idMath::ASin16( v ) ), "ASin16(%.9g) = %g", v, idMath::ASin16( v ) );
	}
	CHECK( IsFinite( idMath::ATan( 0.0f, 0.0f ) ), "ATan(0,0) = %g", idMath::ATan( 0.0f, 0.0f ) );
	// ATan16( 0, 0 ) is not tested: its precondition (asserted) is that x and y are not both zero
}

/*
===============================================================================

	Vectors, matrices and rotations

===============================================================================
*/

TEST( Vec3_NormalizeZero ) {
	idVec3 v( 0.0f, 0.0f, 0.0f );
	const float len = v.Normalize();
	CHECK( IsFinite( v ), "Normalize(0) gives (%s)", v.ToString( 3 ) );
	CHECK( IsFinite( len ), "Normalize(0) returns %g", len );

	idVec3 w( 0.0f, 0.0f, 0.0f );
	const float lenFast = w.NormalizeFast();
	CHECK( IsFinite( w ), "NormalizeFast(0) gives (%s)", w.ToString( 3 ) );
	CHECK( IsFinite( lenFast ), "NormalizeFast(0) returns %g", lenFast );

	idVec3 tiny( 1e-25f, 0.0f, 0.0f );	// square is denormal
	tiny.Normalize();
	CHECK( IsFinite( tiny ), "Normalize(tiny) gives (%s)", tiny.ToString( 3 ) );

	idVec3 normal( 0.0f, 0.0f, 0.0f ), right, up;
	normal.NormalVectors( right, up );
	CHECK( IsFinite( right ) && IsFinite( up ), "NormalVectors of zero: (%s) (%s)", right.ToString( 3 ), up.ToString( 3 ) );
}

TEST( Vec6_NormalizeZero ) {
	// idPhysics_AF::EvaluateContacts normalizes the angular part of the velocity, which is zero for a body at rest
	idVecX dir( 6, VECX_ALLOCA( 6 ) );
	dir.Zero();
	dir.SubVec3( 0 ).Normalize();
	dir.SubVec3( 1 ).Normalize();
	CHECK( IsFinite( dir ), "dir = (%s)", dir.ToString( 3 ) );
}

TEST( Rotation_ZeroAngularVelocity ) {
	// this is what idPhysics_AF::Evolve does for a body that doesn't rotate
	const float timeSteps[] = { 1.0f / 60.0f, 1.0f / 120.0f, 0.008f, 0.009f, 1.0f / 240.0f };
	for ( int t = 0; t < sizeof( timeSteps ) / sizeof( timeSteps[0] ); t++ ) {
		idVec3 vec( 0.0f, 0.0f, 0.0f );
		const float angle = -timeSteps[t] * (float) RAD2DEG( vec.Normalize() );
		idRotation rotation( vec3_origin, vec, angle );
		rotation.Normalize180();
		const idMat3 m = rotation.ToMat3();
		CHECK( IsFinite( angle ), "angle = %g", angle );
		CHECK( IsFinite( m ), "rotation matrix (%s)", m.ToString( 3 ) );

		idMat3 axis = mat3_identity * m;
		axis.OrthoNormalizeSelf();
		CHECK( IsFinite( axis ), "axis (%s)", axis.ToString( 3 ) );
	}
}

TEST( Rotation_SmallAngularVelocity ) {
	idRandom random( 1234 );
	const float speeds[] = { 1e-6f, 1e-4f, 1e-2f, 1.0f, 10.0f, 100.0f };
	for ( int s = 0; s < sizeof( speeds ) / sizeof( speeds[0] ); s++ ) {
		for ( int i = 0; i < 50; i++ ) {
			idVec3 vec( random.CRandomFloat(), random.CRandomFloat(), random.CRandomFloat() );
			vec *= speeds[s];
			const float angle = -( 1.0f / 120.0f ) * (float) RAD2DEG( vec.Normalize() );
			idRotation rotation( vec3_origin, vec, angle );
			rotation.Normalize180();
			idMat3 axis = rotation.ToMat3();
			axis.OrthoNormalizeSelf();
			CHECK( IsFinite( axis ), "speed %g: axis (%s)", speeds[s], axis.ToString( 3 ) );
			CHECK( IsOrthonormal( axis, 1e-4f ), "speed %g: axis (%s)", speeds[s], axis.ToString( 5 ) );
		}
	}
}

TEST( Mat3_ToRotationAndAngularVelocity ) {
	idRandom random( 99 );

	// identity and almost identity matrices are the common case for bodies at rest
	idMat3 m = mat3_identity;
	CHECK( IsFinite( m.ToRotation().GetVec() ) && IsFinite( m.ToRotation().GetAngle() ) );
	CHECK( IsFinite( m.ToAngularVelocity() ), "identity: (%s)", m.ToAngularVelocity().ToString( 3 ) );
	CHECK( IsFinite( m.ToQuat().ToFloatPtr(), 4 ) );

	for ( int i = 0; i < 200; i++ ) {
		const float scale = ( i < 100 ) ? 1e-6f : 1.0f;
		idAngles angles( random.CRandomFloat() * 180.0f * scale, random.CRandomFloat() * 180.0f * scale, random.CRandomFloat() * 180.0f * scale );
		m = angles.ToMat3();
		const idRotation r = m.ToRotation();
		CHECK( IsFinite( r.GetVec() ) && IsFinite( r.GetAngle() ), "angles (%s): rotation (%s) %g", angles.ToString( 6 ), r.GetVec().ToString( 3 ), r.GetAngle() );
		CHECK( IsFinite( m.ToAngularVelocity() ), "angles (%s)", angles.ToString( 6 ) );
		CHECK( r.ToMat3().Compare( m, 1e-3f ), "angles (%s): round trip failed", angles.ToString( 6 ) );
		const idQuat q = m.ToQuat();
		CHECK( IsFinite( q.ToFloatPtr(), 4 ) );
		CHECK( q.ToMat3().Compare( m, 1e-3f ), "angles (%s): quat round trip failed", angles.ToString( 6 ) );
	}
}

TEST( Mat3_OrthoNormalize ) {
	idRandom random( 7 );
	for ( int i = 0; i < 200; i++ ) {
		idAngles angles( random.CRandomFloat() * 180.0f, random.CRandomFloat() * 180.0f, random.CRandomFloat() * 180.0f );
		idMat3 m = angles.ToMat3();
		// add some drift like many integration steps do
		for ( int j = 0; j < 9; j++ ) {
			m.ToFloatPtr()[j] += random.CRandomFloat() * 1e-3f;
		}
		m.OrthoNormalizeSelf();
		CHECK( IsFinite( m ) );
		CHECK( IsOrthonormal( m, 1e-4f ), "(%s)", m.ToString( 5 ) );
	}
}

TEST( Mat3_InverseOfInertiaTensors ) {
	idRandom random( 3 );
	for ( int i = 0; i < 200; i++ ) {
		// symmetric positive definite matrices like inertia tensors
		idMat3 a;
		for ( int j = 0; j < 9; j++ ) {
			a.ToFloatPtr()[j] = random.CRandomFloat();
		}
		const float diagonal = ( i < 100 ) ? 1.0f : 1e-4f;		// small tensors of light bodies
		idMat3 spd = a * a.Transpose() + mat3_identity * diagonal;
		spd *= ( i & 1 ) ? 0.01f : 100.0f;

		const idMat3 inv = spd.Inverse();
		CHECK( IsFinite( inv ), "(%s)", spd.ToString( 5 ) );
		CHECK( ( spd * inv ).Compare( mat3_identity, 1e-2f ), "inverse of (%s) is wrong", spd.ToString( 5 ) );
	}
}

/*
===============================================================================

	Trace models of the articulated figures that go wrong in game/cpuboss
	(env_skeleton, env_gibs_torso, env_gibs_spine)

===============================================================================
*/

struct afBodyDesc_t {
	const char *	name;
	traceModel_t	type;
	idVec3			v1;
	idVec3			v2;
	float			width;		// bone width or number of sides
	float			density;
};

static const afBodyDesc_t afBodies[] = {
	// env_skeleton
	{ "skeleton/waist",		TRM_BOX,	idVec3( -3.5f, -6.5f, -6.0f ),	idVec3( 3.5f, 6.5f, 6.0f ),		0, 0.015f },
	{ "skeleton/chest",		TRM_BOX,	idVec3( -5.5f, -7.0f, -8.5f ),	idVec3( 5.5f, 7.0f, 8.5f ),		0, 0.008f },
	{ "skeleton/head",		TRM_BOX,	idVec3( -3.5f, -3.5f, -5.5f ),	idVec3( 3.5f, 3.5f, 5.5f ),		0, 0.02f },
	{ "skeleton/ruparm",	TRM_BONE,	idVec3( 0, 0, 0 ),				idVec3( 0, -12.0f, -3.0f ),		4, 0.4f },
	{ "skeleton/rloarm",	TRM_CONE,	idVec3( -2.0f, -2.0f, -8.0f ),	idVec3( 2.0f, 2.0f, 8.0f ),		3, 0.2f },
	{ "skeleton/rupleg",	TRM_BONE,	idVec3( 0, 0, 0 ),				idVec3( 0, -2.0f, -20.0f ),		5, 0.2f },
	{ "skeleton/rloleg",	TRM_CONE,	idVec3( -7.0f, -3.0f, -11.0f ),	idVec3( 7.0f, 3.0f, 11.0f ),	3, 0.03f },
	// env_gibs_torso
	{ "torso/waist",		TRM_BOX,	idVec3( -6.0f, -4.5f, -6.5f ),	idVec3( 6.0f, 4.5f, 6.5f ),		0, 0.015f },
	{ "torso/chest",		TRM_BOX,	idVec3( -7.0f, -4.0f, -7.0f ),	idVec3( 7.0f, 4.0f, 7.0f ),		0, 0.015f },
	{ "torso/neck",			TRM_BOX,	idVec3( -3.0f, -3.0f, -3.0f ),	idVec3( 3.0f, 3.0f, 3.0f ),		0, 0.07f },
	// env_gibs_spine
	{ "spine/waist",		TRM_BONE,	idVec3( 0, 0, 0 ),				idVec3( 0, 0, 9.0f ),			6, 0.1f },
	{ "spine/chest",		TRM_BONE,	idVec3( 0, 0, 0 ),				idVec3( 0, 1.0f, 7.0f ),		4, 0.2f },
	{ "spine/shldrs",		TRM_BONE,	idVec3( 0, 0, 0 ),				idVec3( 0, 0.5f, 5.0f ),		3, 0.5f },
};

// builds the trace model the way idAF::LoadBody does, returns the bone axis
static idMat3 SetupAFTraceModel( const afBodyDesc_t &desc, idTraceModel &trm ) {
	idMat3 axis = mat3_identity;
	idBounds bounds( desc.v1, desc.v2 );
	switch ( desc.type ) {
		case TRM_BOX:
			trm.SetupBox( bounds );
			break;
		case TRM_CONE:
			bounds[0].z -= bounds[1].z;
			bounds[1].z = 0.0f;
			trm.SetupCone( bounds, (int)desc.width );
			break;
		case TRM_BONE: {
			axis[2] = desc.v2 - desc.v1;
			const float length = axis[2].Normalize();
			axis[2].NormalVectors( axis[0], axis[1] );
			axis[1] = -axis[1];
			trm.SetupBone( length, desc.width );
			break;
		}
		default:
			break;
	}
	return axis;
}

TEST( TraceModel_AFBodyMassProperties ) {
	for ( int i = 0; i < sizeof( afBodies ) / sizeof( afBodies[0] ); i++ ) {
		const afBodyDesc_t &desc = afBodies[i];
		idTraceModel trm;
		const idMat3 axis = SetupAFTraceModel( desc, trm );
		CHECK( IsFinite( axis ), "%s: bone axis (%s)", desc.name, axis.ToString( 3 ) );
		CHECK( trm.isConvex, "%s: not convex", desc.name );

		// idAF::LoadBody: unit density, then move the center of mass to the origin
		float mass;
		idVec3 centerOfMass;
		idMat3 inertiaTensor;
		trm.GetMassProperties( 1.0f, mass, centerOfMass, inertiaTensor );
		CHECK( IsFinite( mass ) && mass > 0.0f, "%s: unit mass %g", desc.name, mass );
		CHECK( IsFinite( centerOfMass ), "%s: center of mass (%s)", desc.name, centerOfMass.ToString( 3 ) );
		trm.Translate( -centerOfMass );

		// idAFBody::SetDensity
		trm.GetMassProperties( desc.density, mass, centerOfMass, inertiaTensor );
		CHECK( IsFinite( mass ) && mass > 0.0f, "%s: mass %g", desc.name, mass );
		CHECK( centerOfMass.Compare( vec3_origin, 0.1f ), "%s: center of mass not at origin (%s)", desc.name, centerOfMass.ToString( 3 ) );
		CHECK( IsFinite( inertiaTensor ), "%s: inertia (%s)", desc.name, inertiaTensor.ToString( 3 ) );
		CHECK( inertiaTensor[0][0] > 0.0f && inertiaTensor[1][1] > 0.0f && inertiaTensor[2][2] > 0.0f,
				"%s: inertia diagonal not positive (%s)", desc.name, inertiaTensor.ToString( 5 ) );

		idMat3 inverseInertia;
		if ( inertiaTensor.IsDiagonal( 1e-3f ) ) {
			inverseInertia.Identity();
			inverseInertia[0][0] = 1.0f / inertiaTensor[0][0];
			inverseInertia[1][1] = 1.0f / inertiaTensor[1][1];
			inverseInertia[2][2] = 1.0f / inertiaTensor[2][2];
		} else {
			inverseInertia = inertiaTensor.Inverse();
		}
		CHECK( IsFinite( inverseInertia ), "%s: inverse inertia (%s) of (%s)", desc.name, inverseInertia.ToString( 3 ), inertiaTensor.ToString( 5 ) );
		CHECK( IsFinite( 1.0f / mass ), "%s: inverse mass", desc.name );

		// sanity check against the bounding box of the trace model
		const idVec3 size = trm.bounds[1] - trm.bounds[0];
		const float boxMass = size.x * size.y * size.z * desc.density;
		CHECK( mass <= boxMass * 1.001f && mass > boxMass * 0.05f, "%s: mass %g, bounding box mass %g", desc.name, mass, boxMass );
	}
}

TEST( TraceModel_ShapesMassProperties ) {
	const idBounds bounds( idVec3( -4, -3, -6 ), idVec3( 4, 3, 6 ) );
	for ( int shape = 0; shape < 6; shape++ ) {
		for ( int sides = 3; sides <= 10; sides++ ) {
			idTraceModel trm;
			const char *name = "";
			switch ( shape ) {
				case 0: trm.SetupBox( bounds ); name = "box"; break;
				case 1: trm.SetupOctahedron( bounds ); name = "octahedron"; break;
				case 2: trm.SetupDodecahedron( bounds ); name = "dodecahedron"; break;
				case 3: trm.SetupCylinder( bounds, sides ); name = "cylinder"; break;
				case 4: trm.SetupCone( bounds, sides ); name = "cone"; break;
				case 5: trm.SetupBone( 10.0f + sides, 2.0f + sides * 0.5f ); name = "bone"; break;
			}
			float mass;
			idVec3 centerOfMass;
			idMat3 inertiaTensor;
			trm.GetMassProperties( 0.01f, mass, centerOfMass, inertiaTensor );
			CHECK( IsFinite( mass ) && mass > 0.0f, "%s %d: mass %g", name, sides, mass );
			CHECK( IsFinite( centerOfMass ), "%s %d", name, sides );
			CHECK( IsFinite( inertiaTensor ), "%s %d: inertia (%s)", name, sides, inertiaTensor.ToString( 3 ) );
			CHECK( IsFinite( inertiaTensor.Inverse() ), "%s %d: inverse inertia of (%s)", name, sides, inertiaTensor.ToString( 5 ) );
			if ( shape < 3 ) {
				break;		// the number of sides only matters for cylinders, cones and bones
			}
		}
	}
}

/*
===============================================================================

	idMatX / idVecX

===============================================================================
*/

static void MakeSPD( idMatX &m, int n, int seed, float diagonal ) {
	idMatX a;
	a.Random( n, n, seed, -1.0f, 1.0f );
	m.SetSize( n, n );
	for ( int i = 0; i < n; i++ ) {
		for ( int j = 0; j < n; j++ ) {
			double sum = 0.0;
			for ( int k = 0; k < n; k++ ) {
				sum += (double)a[i][k] * a[j][k];
			}
			m[i][j] = (float)sum + ( ( i == j ) ? diagonal : 0.0f );
		}
	}
}

// residual of m * x = b, computed in double precision
static float Residual( const idMatX &m, const idVecX &x, const idVecX &b ) {
	double maxError = 0.0;
	for ( int i = 0; i < m.GetNumRows(); i++ ) {
		double sum = 0.0;
		for ( int j = 0; j < m.GetNumColumns(); j++ ) {
			sum += (double)m[i][j] * x[j];
		}
		maxError = Max( maxError, fabs( sum - b[i] ) );
	}
	return (float)maxError;
}

TEST( MatX_MultiplyMatchesReference ) {
	for ( int rows = 1; rows <= 24; rows++ ) {
		for ( int cols = 1; cols <= 24; cols += ( cols < 8 ) ? 1 : 3 ) {
			idMatX m;
			m.Random( rows, cols, rows * 100 + cols, -1.0f, 1.0f );
			idVecX v, vt;
			v.SetSize( cols );
			v.Random( rows + cols, -1.0f, 1.0f );
			vt.SetSize( rows );
			vt.Random( rows * cols, -1.0f, 1.0f );

			idVecX dst, dstT;
			dst.SetSize( rows );
			dstT.SetSize( cols );
			m.Multiply( dst, v );
			m.TransposeMultiply( dstT, vt );

			for ( int i = 0; i < rows; i++ ) {
				double sum = 0.0;
				for ( int j = 0; j < cols; j++ ) {
					sum += (double)m[i][j] * v[j];
				}
				CHECK( Near( dst[i], (float)sum, 1e-4f ), "%dx%d row %d: %g vs %g", rows, cols, i, dst[i], sum );
			}
			for ( int j = 0; j < cols; j++ ) {
				double sum = 0.0;
				for ( int i = 0; i < rows; i++ ) {
					sum += (double)m[i][j] * vt[i];
				}
				CHECK( Near( dstT[j], (float)sum, 1e-4f ), "%dx%d transposed column %d: %g vs %g", rows, cols, j, dstT[j], sum );
			}
		}
	}
}

TEST( MatX_PaddingDoesNotLeak ) {
	// fill the allocation with NaN first: SIMD code that reads past the end of a row or vector must not pick it up
	for ( int n = 1; n <= 13; n++ ) {
		float *matMem = (float *)Mem_Alloc16( ( n * n + 8 ) * sizeof( float ), TAG_MATH );
		float *vecMem = (float *)Mem_Alloc16( ( n + 8 ) * sizeof( float ), TAG_MATH );
		float *dstMem = (float *)Mem_Alloc16( ( n + 8 ) * sizeof( float ), TAG_MATH );
		const unsigned int nanBits = 0x7FC00000;
		const float nan = *(const float *)&nanBits;
		for ( int i = 0; i < n * n + 8; i++ ) {
			matMem[i] = nan;
		}
		for ( int i = 0; i < n + 8; i++ ) {
			vecMem[i] = nan;
			dstMem[i] = nan;
		}

		idMatX m;
		m.SetData( n, n, matMem );
		idVecX v, dst;
		v.SetData( n, vecMem );
		dst.SetData( n, dstMem );
		for ( int i = 0; i < n; i++ ) {
			v[i] = 1.0f + i;
			for ( int j = 0; j < n; j++ ) {
				m[i][j] = ( i == j ) ? 2.0f : 0.1f;
			}
		}
		m.Multiply( dst, v );
		CHECK( IsFinite( dst ), "n = %d: Multiply picked up padding (%s)", n, dst.ToString( 2 ) );
		m.TransposeMultiply( dst, v );
		CHECK( IsFinite( dst ), "n = %d: TransposeMultiply picked up padding (%s)", n, dst.ToString( 2 ) );

		CHECK( m.LDLT_Factor(), "n = %d", n );
		m.LDLT_Solve( dst, v );
		CHECK( IsFinite( dst ), "n = %d: LDLT_Solve picked up padding (%s)", n, dst.ToString( 2 ) );

		Mem_Free16( matMem );
		Mem_Free16( vecMem );
		Mem_Free16( dstMem );
	}
}

TEST( MatX_FactorAndSolve ) {
	for ( int n = 1; n <= 40; n++ ) {
		for ( int variant = 0; variant < 2; variant++ ) {
			const float diagonal = variant == 0 ? 1.0f : 1e-3f;
			idMatX spd;
			MakeSPD( spd, n, n * 31 + variant, diagonal );
			idVecX b, x;
			b.SetSize( n );
			b.Random( n, -1.0f, 1.0f );
			x.SetSize( n );

			idMatX ldlt = spd;
			CHECK( ldlt.LDLT_Factor(), "n = %d variant %d: LDLT_Factor failed", n, variant );
			ldlt.LDLT_Solve( x, b );
			CHECK( IsFinite( x ), "n = %d variant %d: LDLT_Solve not finite", n, variant );
			CHECK( Residual( spd, x, b ) < 1e-2f, "n = %d variant %d: LDLT residual %g", n, variant, Residual( spd, x, b ) );

			idMatX chol = spd;
			CHECK( chol.Cholesky_Factor(), "n = %d variant %d: Cholesky_Factor failed", n, variant );
			chol.Cholesky_Solve( x, b );
			CHECK( IsFinite( x ), "n = %d variant %d: Cholesky_Solve not finite", n, variant );
			CHECK( Residual( spd, x, b ) < 1e-2f, "n = %d variant %d: Cholesky residual %g", n, variant, Residual( spd, x, b ) );

			idMatX lu = spd;
			idList<int> index;
			index.SetNum( n );
			CHECK( lu.LU_Factor( index.Ptr() ), "n = %d variant %d: LU_Factor failed", n, variant );
			lu.LU_Solve( x, b, index.Ptr() );
			CHECK( IsFinite( x ), "n = %d variant %d: LU_Solve not finite", n, variant );
			CHECK( Residual( spd, x, b ) < 1e-2f, "n = %d variant %d: LU residual %g", n, variant, Residual( spd, x, b ) );
		}
	}
}

/*
===============================================================================

	LCP solvers

	The auxiliary constraints of articulated figures (joint limits and
	contacts) are solved with idLCP. A valid solution of
	  A * x = b + w,  lo <= x <= hi
	satisfies for every row:  x == lo && w >= 0  or  x == hi && w <= 0  or  lo < x < hi && w == 0

===============================================================================
*/

static void CheckLCPSolution( const char *solver, int n, int seed, const idMatX &A, const idVecX &x, const idVecX &b,
							const idVecX &lo, const idVecX &hi, const int *boxIndex ) {
	CHECK( IsFinite( x ), "%s n = %d seed %d: solution not finite (%s)", solver, n, seed, x.ToString( 3 ) );
	if ( !IsFinite( x ) ) {
		return;
	}
	for ( int i = 0; i < n; i++ ) {
		float l = lo[i];
		float h = hi[i];
		if ( boxIndex != NULL && boxIndex[i] >= 0 ) {
			l *= idMath::Fabs( x[boxIndex[i]] );
			h *= idMath::Fabs( x[boxIndex[i]] );
		}
		double w = -b[i];
		for ( int j = 0; j < n; j++ ) {
			w += (double)A[i][j] * x[j];
		}
		const float tolerance = 1e-2f * Max( 1.0f, idMath::Fabs( b[i] ) );
		CHECK( x[i] >= l - tolerance && x[i] <= h + tolerance, "%s n = %d seed %d row %d: x %g outside [%g, %g]", solver, n, seed, i, x[i], l, h );
		const bool atLo = x[i] <= l + tolerance;
		const bool atHi = x[i] >= h - tolerance;
		const bool ok = ( atLo && w >= -tolerance ) || ( atHi && w <= tolerance ) || fabs( w ) <= tolerance;
		CHECK( ok, "%s n = %d seed %d row %d: x %g in [%g, %g] but w = %g", solver, n, seed, i, x[i], l, h, w );
	}
}

// reference solver: projected Gauss-Seidel in double precision, converges to the unique solution for symmetric positive definite A
static void ReferenceLCP( const idMatX &A, const idVecX &b, const idVecX &lo, const idVecX &hi, const int *boxIndex, double *x ) {
	const int n = A.GetNumRows();
	for ( int i = 0; i < n; i++ ) {
		x[i] = 0.0;
	}
	for ( int iteration = 0; iteration < 20000; iteration++ ) {
		double maxChange = 0.0;
		for ( int i = 0; i < n; i++ ) {
			double r = b[i];
			for ( int j = 0; j < n; j++ ) {
				if ( j != i ) {
					r -= (double)A[i][j] * x[j];
				}
			}
			double v = r / A[i][i];
			double l = lo[i], h = hi[i];
			if ( boxIndex != NULL && boxIndex[i] >= 0 ) {
				l = -fabs( lo[i] * x[boxIndex[i]] );
				h = fabs( hi[i] * x[boxIndex[i]] );
			}
			v = Max( l, Min( h, v ) );
			maxChange = Max( maxChange, fabs( v - x[i] ) );
			x[i] = v;
		}
		if ( maxChange < 1e-9 ) {
			break;
		}
	}
}

enum lcpLayout_t {
	LCP_UNPADDED,		// n x n
	LCP_PADDED_ZERO,	// rows padded to a multiple of 4 floats, padding is zero
	LCP_PADDED_NAN		// rows padded, padding is NaN like uninitialized _alloca memory can be (idPhysics_AF::AuxiliaryForces)
};

static const char *lcpLayoutNames[] = { "unpadded", "padded", "padded-nan" };

// copies src into a matrix with the given row layout, the memory must be freed with Mem_Free16
static float *MakeLCPMatrix( idMatX &dst, const idMatX &src, lcpLayout_t layout ) {
	const int n = src.GetNumRows();
	const int columns = ( layout == LCP_UNPADDED ) ? n : ( ( n + 3 ) & ~3 );
	float *mem = (float *)Mem_Alloc16( ( n * columns + 4 ) * sizeof( float ), TAG_MATH );
	const unsigned int nanBits = 0x7FC00000;
	const float padding = ( layout == LCP_PADDED_NAN ) ? *(const float *)&nanBits : 0.0f;
	for ( int i = 0; i < n * columns + 4; i++ ) {
		mem[i] = padding;
	}
	dst.SetData( n, columns, mem );
	for ( int i = 0; i < n; i++ ) {
		for ( int j = 0; j < n; j++ ) {
			dst[i][j] = src[i][j];
		}
	}
	return mem;
}

static void RunLCPTests( bool symmetric, lcpLayout_t layout, bool generic = false ) {
	idStr solverName = va( "%s%s %s", generic ? "generic " : "", symmetric ? "symmetric" : "square", lcpLayoutNames[layout] );
	const char *solver = solverName.c_str();
	for ( int n = 1; n <= 32; n++ ) {
		for ( int seed = 0; seed < 4; seed++ ) {
			idRandom random( n * 1000 + seed );

			idMatX spd;
			MakeSPD( spd, n, n * 7 + seed, 0.5f );
			idMatX A;
			float *matrixMemory = MakeLCPMatrix( A, spd, layout );
			idVecX x, b, lo, hi;
			x.SetSize( n );
			b.SetSize( n );
			lo.SetSize( n );
			hi.SetSize( n );
			idList<int> boxIndex;
			boxIndex.SetNum( n );

			// a mix of unbounded rows (joints), contact rows (x >= 0) and friction rows bounded by a contact row
			for ( int i = 0; i < n; i++ ) {
				b[i] = random.CRandomFloat() * 10.0f;
				boxIndex[i] = -1;
				switch ( ( i + seed ) % 4 ) {
					case 0:
						lo[i] = -idMath::INFINITY;
						hi[i] = idMath::INFINITY;
						break;
					case 1:
						lo[i] = 0.0f;
						hi[i] = idMath::INFINITY;
						break;
					case 2:
						lo[i] = -1.0f;
						hi[i] = 1.0f;
						break;
					case 3:
						lo[i] = -0.5f;
						hi[i] = 0.5f;
						if ( i > 0 && ( i - 1 + seed ) % 4 == 2 ) {
							// friction scaled by the force of the contact row before it
						} else if ( i >= 2 && ( i - 2 + seed ) % 4 == 1 ) {
							boxIndex[i] = i - 2;
						}
						break;
				}
			}

			idLCP *lcpSSE = NULL;
			idLCP_Generic *lcpGeneric = NULL;
			if ( generic ) {
				lcpGeneric = symmetric ? idLCP_Generic::AllocSymmetric() : idLCP_Generic::AllocSquare();
			} else {
				lcpSSE = symmetric ? idLCP::AllocSymmetric() : idLCP::AllocSquare();
			}
			#define LCP_SOLVE( ... ) ( generic ? lcpGeneric->Solve( __VA_ARGS__ ) : lcpSSE->Solve( __VA_ARGS__ ) )
			x.Zero();
			const bool solved = LCP_SOLVE( A, x, b, lo, hi, boxIndex.Ptr() );
			CHECK( solved, "%s n = %d seed %d: Solve failed", solver, n, seed );
			CheckLCPSolution( solver, n, seed, spd, x, b, lo, hi, boxIndex.Ptr() );

			// Solve() leaves the columns of the matrix permuted (the rows are only permuted through pointers),
			// it must still be a permutation of the original: every row keeps the same set of values
			{
				int mismatches = 0;
				for ( int i = 0; i < n; i++ ) {
					double sumA = 0.0, sumSPD = 0.0;
					for ( int j = 0; j < n; j++ ) {
						sumA += A[i][j];
						sumSPD += spd[i][j];
					}
					if ( fabs( sumA - sumSPD ) > 1e-3 * Max( 1.0, fabs( sumSPD ) ) ) {
						mismatches++;
					}
				}
				CHECK( mismatches == 0, "%s n = %d seed %d: %d matrix rows are not a permutation of the input", solver, n, seed, mismatches );
			}

			// compare with the reference solution
			{
				idTempArray<double> reference( n );
				ReferenceLCP( spd, b, lo, hi, boxIndex.Ptr(), reference.Ptr() );
				double maxDiff = 0.0;
				int worst = 0;
				for ( int i = 0; i < n; i++ ) {
					const double diff = fabs( x[i] - reference[i] ) / Max( 1.0, fabs( reference[i] ) );
					if ( diff > maxDiff ) {
						maxDiff = diff;
						worst = i;
					}
				}
				CHECK( maxDiff < 1e-2, "%s n = %d seed %d: x[%d] = %g, reference %g", solver, n, seed, worst, x[worst], reference[worst] );
			}

			// warm start with the previous solution, as the AF code does every frame
			// restore the matrix in case Solve changed it
			for ( int i = 0; i < n; i++ ) {
				for ( int j = 0; j < n; j++ ) {
					A[i][j] = spd[i][j];
				}
			}
			const bool solvedAgain = LCP_SOLVE( A, x, b, lo, hi, boxIndex.Ptr() );
			CHECK( solvedAgain, "%s n = %d seed %d: warm started Solve failed", solver, n, seed );
			CheckLCPSolution( solver, n, seed, spd, x, b, lo, hi, boxIndex.Ptr() );
			#undef LCP_SOLVE
			delete lcpSSE;
			delete lcpGeneric;
			Mem_Free16( matrixMemory );
		}
	}
}

TEST( LCP_Square ) {
	RunLCPTests( false, LCP_UNPADDED );
	RunLCPTests( false, LCP_PADDED_ZERO );
	RunLCPTests( false, LCP_PADDED_NAN );
}

TEST( LCP_Symmetric ) {
	RunLCPTests( true, LCP_UNPADDED );
	RunLCPTests( true, LCP_PADDED_ZERO );
	RunLCPTests( true, LCP_PADDED_NAN );
}

TEST( LCP_GenericSquare ) {
	RunLCPTests( false, LCP_UNPADDED, true );
	RunLCPTests( false, LCP_PADDED_ZERO, true );
	RunLCPTests( false, LCP_PADDED_NAN, true );
}

TEST( LCP_GenericSymmetric ) {
	RunLCPTests( true, LCP_UNPADDED, true );
	RunLCPTests( true, LCP_PADDED_ZERO, true );
	RunLCPTests( true, LCP_PADDED_NAN, true );
}

// one kind of bounds at a time, to see which part of the solver fails
TEST( LCP_BoundKinds ) {
	const char *kindNames[] = { "unbounded", "lower bound 0", "box [-1, 1]", "mixed unbounded + lower" };
	for ( int symmetric = 0; symmetric < 2; symmetric++ ) {
		for ( int kind = 0; kind < 4; kind++ ) {
			for ( int n = 1; n <= 24; n++ ) {
				idMatX spd;
				MakeSPD( spd, n, n * 13 + kind, 0.5f );
				idMatX A;
				float *mem = MakeLCPMatrix( A, spd, LCP_PADDED_ZERO );
				idVecX x, b, lo, hi;
				x.SetSize( n ); b.SetSize( n ); lo.SetSize( n ); hi.SetSize( n );
				idRandom random( n + kind * 100 );
				for ( int i = 0; i < n; i++ ) {
					b[i] = random.CRandomFloat() * 10.0f;
					const bool lower = ( kind == 1 ) || ( kind == 3 && ( i & 1 ) );
					lo[i] = ( kind == 2 ) ? -1.0f : lower ? 0.0f : -idMath::INFINITY;
					hi[i] = ( kind == 2 ) ? 1.0f : idMath::INFINITY;
				}
				x.Zero();
				idLCP *lcp = symmetric ? idLCP::AllocSymmetric() : idLCP::AllocSquare();
				lcp->Solve( A, x, b, lo, hi );
				delete lcp;
				idTempArray<double> reference( n );
				ReferenceLCP( spd, b, lo, hi, NULL, reference.Ptr() );
				double maxDiff = 0.0;
				int worst = 0;
				for ( int i = 0; i < n; i++ ) {
					const double diff = fabs( x[i] - reference[i] ) / Max( 1.0, fabs( reference[i] ) );
					if ( diff > maxDiff ) {
						maxDiff = diff;
						worst = i;
					}
				}
				CHECK( maxDiff < 1e-2, "%s %s n = %d: x[%d] = %g, reference %g", symmetric ? "symmetric" : "square", kindNames[kind], n, worst, x[worst], reference[worst] );
				Mem_Free16( mem );
			}
		}
	}
}

TEST( LCP_SimpleKnownSolutions ) {
	// A = 2 * I, b = (2, -2, 4): unbounded -> x = (1, -1, 2), x >= 0 -> x = (1, 0, 2), |x| <= 0.5 -> (0.5, -0.5, 0.5)
	for ( int symmetric = 0; symmetric < 2; symmetric++ ) {
		for ( int layout = 0; layout < 3; layout++ ) {
			for ( int bounds = 0; bounds < 3; bounds++ ) {
				const int n = 3;
				idMatX identity;
				identity.SetSize( n, n );
				identity.Identity();
				identity *= 2.0f;
				idMatX A;
				float *mem = MakeLCPMatrix( A, identity, (lcpLayout_t)layout );
				idVecX x, b, lo, hi;
				x.SetSize( n ); b.SetSize( n ); lo.SetSize( n ); hi.SetSize( n );
				b[0] = 2.0f; b[1] = -2.0f; b[2] = 4.0f;
				const float expected[3][3] = { { 1, -1, 2 }, { 1, 0, 2 }, { 0.5f, -0.5f, 0.5f } };
				for ( int i = 0; i < n; i++ ) {
					lo[i] = ( bounds == 0 ) ? -idMath::INFINITY : ( bounds == 1 ) ? 0.0f : -0.5f;
					hi[i] = ( bounds == 2 ) ? 0.5f : idMath::INFINITY;
				}
				x.Zero();
				idLCP *lcp = symmetric ? idLCP::AllocSymmetric() : idLCP::AllocSquare();
				const bool solved = lcp->Solve( A, x, b, lo, hi );
				delete lcp;
				CHECK( solved, "%s %s bounds %d: Solve failed", symmetric ? "symmetric" : "square", lcpLayoutNames[layout], bounds );
				for ( int i = 0; i < n; i++ ) {
					CHECK( Near( x[i], expected[bounds][i], 1e-4f ), "%s %s bounds %d: x[%d] = %g, expected %g",
							symmetric ? "symmetric" : "square", lcpLayoutNames[layout], bounds, i, x[i], expected[bounds][i] );
				}
				Mem_Free16( mem );
			}
		}
	}
}

TEST( LCP_DegenerateSystems ) {
	// singular / nearly singular systems happen when constraints are redundant (two joints locking the same axis)
	for ( int symmetric = 0; symmetric < 2; symmetric++ ) {
		for ( int n = 2; n <= 12; n++ ) {
			idMatX A;
			A.SetSize( n, n );
			for ( int i = 0; i < n; i++ ) {
				for ( int j = 0; j < n; j++ ) {
					A[i][j] = 1.0f + ( ( i == j ) ? 1e-6f : 0.0f );	// rank one plus a tiny diagonal
				}
			}
			idVecX x, b, lo, hi;
			x.SetSize( n );
			b.SetSize( n );
			lo.SetSize( n );
			hi.SetSize( n );
			for ( int i = 0; i < n; i++ ) {
				b[i] = 1.0f;
				lo[i] = 0.0f;
				hi[i] = idMath::INFINITY;
			}
			x.Zero();
			idLCP *lcp = symmetric ? idLCP::AllocSymmetric() : idLCP::AllocSquare();
			lcp->Solve( A, x, b, lo, hi );
			CHECK( IsFinite( x ), "%s n = %d: solution of a degenerate system is not finite (%s)", symmetric ? "symmetric" : "square", n, x.ToString( 3 ) );
			delete lcp;
		}
	}
}

/*
===============================================================================

	SIMD processors: the SSE implementation must match the generic one

===============================================================================
*/

static void MakeJoints( idJointQuat *joints, int num, int seed ) {
	idRandom random( seed );
	for ( int i = 0; i < num; i++ ) {
		idAngles angles( random.CRandomFloat() * 180.0f, random.CRandomFloat() * 180.0f, random.CRandomFloat() * 180.0f );
		joints[i].q = angles.ToQuat();
		joints[i].t = idVec3( random.CRandomFloat() * 20.0f, random.CRandomFloat() * 20.0f, random.CRandomFloat() * 20.0f );
		joints[i].w = 0.0f;
	}
}

static bool JointMatsMatch( const idJointMat &a, const idJointMat &b, float epsilon ) {
	const float *pa = a.ToFloatPtr();
	const float *pb = b.ToFloatPtr();
	for ( int i = 0; i < 12; i++ ) {
		if ( !IsFinite( pa[i] ) || !IsFinite( pb[i] ) || !Near( pa[i], pb[i], epsilon ) ) {
			return false;
		}
	}
	return true;
}

TEST( SIMD_JointsMatchGeneric ) {
	idSIMDProcessor *sse = sseProcessor;
	idSIMDProcessor *generic = genericProcessor;

	const int MAX_JOINTS = 67;
	idTempArray<idJointQuat> quats( MAX_JOINTS );
	idTempArray<idJointQuat> blendQuats( MAX_JOINTS );
	idTempArray<idJointMat> matsSSE( MAX_JOINTS );
	idTempArray<idJointMat> matsGeneric( MAX_JOINTS );
	idTempArray<int> index( MAX_JOINTS );
	idTempArray<int> parents( MAX_JOINTS );

	for ( int num = 1; num <= MAX_JOINTS; num += 3 ) {
		MakeJoints( quats.Ptr(), num, num );
		MakeJoints( blendQuats.Ptr(), num, num + 1000 );

		// quats -> mats
		sse->ConvertJointQuatsToJointMats( matsSSE.Ptr(), quats.Ptr(), num );
		generic->ConvertJointQuatsToJointMats( matsGeneric.Ptr(), quats.Ptr(), num );
		for ( int i = 0; i < num; i++ ) {
			CHECK( JointMatsMatch( matsSSE[i], matsGeneric[i], 1e-4f ), "ConvertJointQuatsToJointMats num %d joint %d", num, i );
		}

		// mats -> quats round trip
		idTempArray<idJointQuat> backSSE( num );
		sse->ConvertJointMatsToJointQuats( backSSE.Ptr(), matsGeneric.Ptr(), num );
		for ( int i = 0; i < num; i++ ) {
			const idQuat &a = backSSE[i].q;
			const idQuat &b = quats[i].q;
			const bool same = idMath::Fabs( a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w ) > 0.999f;
			CHECK( IsFinite( a.ToFloatPtr(), 4 ) && same, "ConvertJointMatsToJointQuats num %d joint %d", num, i );
		}

		// hierarchy transform
		for ( int i = 0; i < num; i++ ) {
			parents[i] = ( i == 0 ) ? 0 : ( i - 1 ) / 2;
		}
		memcpy( matsSSE.Ptr(), matsGeneric.Ptr(), num * sizeof( idJointMat ) );
		sse->TransformJoints( matsSSE.Ptr(), parents.Ptr(), 1, num - 1 );
		generic->TransformJoints( matsGeneric.Ptr(), parents.Ptr(), 1, num - 1 );
		for ( int i = 0; i < num; i++ ) {
			CHECK( JointMatsMatch( matsSSE[i], matsGeneric[i], 1e-3f ), "TransformJoints num %d joint %d", num, i );
		}
		sse->UntransformJoints( matsSSE.Ptr(), parents.Ptr(), 1, num - 1 );
		generic->UntransformJoints( matsGeneric.Ptr(), parents.Ptr(), 1, num - 1 );
		for ( int i = 0; i < num; i++ ) {
			CHECK( JointMatsMatch( matsSSE[i], matsGeneric[i], 1e-3f ), "UntransformJoints num %d joint %d", num, i );
		}

		// blending
		for ( int i = 0; i < num; i++ ) {
			index[i] = i;
		}
		const float lerps[] = { 0.0f, 0.25f, 0.5f, 1.0f };
		for ( int l = 0; l < 4; l++ ) {
			idTempArray<idJointQuat> blendSSE( num );
			idTempArray<idJointQuat> blendGeneric( num );
			memcpy( blendSSE.Ptr(), quats.Ptr(), num * sizeof( idJointQuat ) );
			memcpy( blendGeneric.Ptr(), quats.Ptr(), num * sizeof( idJointQuat ) );
			sse->BlendJoints( blendSSE.Ptr(), blendQuats.Ptr(), lerps[l], index.Ptr(), num );
			generic->BlendJoints( blendGeneric.Ptr(), blendQuats.Ptr(), lerps[l], index.Ptr(), num );
			for ( int i = 0; i < num; i++ ) {
				const idQuat &a = blendSSE[i].q;
				const idQuat &b = blendGeneric[i].q;
				const bool same = idMath::Fabs( a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w ) > 0.999f;
				CHECK( IsFinite( a.ToFloatPtr(), 4 ) && same, "BlendJoints lerp %g num %d joint %d", lerps[l], num, i );
				CHECK( blendSSE[i].t.Compare( blendGeneric[i].t, 1e-3f ), "BlendJoints lerp %g num %d joint %d translation", lerps[l], num, i );
			}
		}
	}

	// identical joints must blend to the same joint, not to NaN (slerp of an angle of zero)
	{
		idJointQuat a, b;
		a.q = idAngles( 10, 20, 30 ).ToQuat();
		a.t = idVec3( 1, 2, 3 );
		a.w = 0.0f;
		b = a;
		int zero = 0;
		for ( int p = 0; p < 2; p++ ) {
			idSIMDProcessor *proc = ( p == 0 ) ? sse : generic;
			idJointQuat r = a;
			proc->BlendJoints( &r, &b, 0.5f, &zero, 1 );
			CHECK( IsFinite( r.q.ToFloatPtr(), 4 ), "%s BlendJoints of identical joints gives NaN", proc->GetName() );
		}
	}
}

/*
===============================================================================

	Render interpolation (renderer/RenderInterpolation.h): the axis lerp of a def drawn
	between two tic poses, and the decision to draw a jump at the tic pose instead

===============================================================================
*/

static idMat3 YawAxis( const float yaw ) {
	return idAngles( 0.0f, yaw, 0.0f ).ToMat3();
}

static idMat3 RowLerp( const idMat3 & from, const idMat3 & to, const float t ) {
	return from + ( to - from ) * t;
}

TEST( Interp_IsRotationAxis ) {
	CHECK( R_IsRotationAxis( mat3_identity ) );
	CHECK( R_IsRotationAxis( idAngles( 30.0f, -120.0f, 45.0f ).ToMat3() ) );
	CHECK( !R_IsRotationAxis( YawAxis( 20.0f ) * 0.7f ), "scaled (g_gunScale)" );

	idMat3 mirrored = YawAxis( 20.0f );
	mirrored[1] = -mirrored[1];
	CHECK( !R_IsRotationAxis( mirrored ), "mirrored" );

	idMat3 sheared = mat3_identity;
	sheared[0][1] = 0.1f;
	CHECK( !R_IsRotationAxis( sheared ), "sheared" );

	CHECK( !R_IsRotationAxis( mat3_zero ), "zero" );

	idMat3 nan = mat3_identity;
	nan[2][0] = std::numeric_limits<float>::quiet_NaN();
	CHECK( !R_IsRotationAxis( nan ), "NaN" );
}

TEST( Interp_LerpAxisRotation ) {
	const idMat3 from = idAngles( 10.0f, 20.0f, 30.0f ).ToMat3();
	const idMat3 to = idAngles( -40.0f, 75.0f, 5.0f ).ToMat3();

	CHECK( R_LerpAxis( from, to, 0.0f ).Compare( from, 1e-5f ), "t = 0 is from" );
	CHECK( R_LerpAxis( from, to, 1.0f ).Compare( to, 1e-5f ), "t = 1 is to" );
	CHECK( R_LerpAxis( from, from, 0.37f ).Compare( from, 1e-5f ), "equal axes" );

	for ( int i = 0; i <= 20; i++ ) {
		const float t = i / 20.0f;
		const idMat3 m = R_LerpAxis( from, to, t );
		CHECK( IsFinite( m ) && IsOrthonormal( m, 1e-4f ), "t %.2f", t );
	}

	// a tiny turn goes through the plain lerp in Slerp and must still come back orthonormal
	const idMat3 tiny = R_LerpAxis( YawAxis( 0.0f ), YawAxis( 0.01f ), 0.5f );
	CHECK( IsFinite( tiny ) && IsOrthonormal( tiny, 1e-5f ), "tiny turn" );
}

TEST( Interp_LerpAxisAngles ) {
	// constant angular speed: a quarter and half of 80 degrees of yaw
	CHECK( Near( R_LerpAxis( YawAxis( 0.0f ), YawAxis( 80.0f ), 0.25f ).ToAngles().yaw, 20.0f, 0.05f / 20.0f ) );
	CHECK( Near( R_LerpAxis( YawAxis( 0.0f ), YawAxis( 80.0f ), 0.5f ).ToAngles().yaw, 40.0f, 0.05f / 40.0f ) );

	// the short way across +-180: 170 -> -170 passes 180, not 0
	const float half = R_LerpAxis( YawAxis( 170.0f ), YawAxis( -170.0f ), 0.5f ).ToAngles().yaw;
	CHECK( Near( idMath::Fabs( half ), 180.0f, 0.05f / 180.0f ), "yaw %f", half );
	const float quarter = idMath::AngleNormalize180( R_LerpAxis( YawAxis( 170.0f ), YawAxis( -170.0f ), 0.25f ).ToAngles().yaw );
	CHECK( Near( quarter, 175.0f, 0.05f / 175.0f ), "yaw %f", quarter );

	// pitch and roll together
	const idAngles mid = R_LerpAxis( idAngles( 0.0f, 0.0f, 0.0f ).ToMat3(), idAngles( 30.0f, 0.0f, 0.0f ).ToMat3(), 0.5f ).ToAngles();
	CHECK( Near( mid.pitch, 15.0f, 0.05f / 15.0f ) && idMath::Fabs( mid.yaw ) < 0.05f && idMath::Fabs( mid.roll ) < 0.05f,
		"%f %f %f", mid.pitch, mid.yaw, mid.roll );
}

TEST( Interp_LerpAxisNotRotation ) {
	// a scaled axis (the view weapon with g_gunScale) and a mirrored one fall back to the row lerp
	const idMat3 scaledFrom = YawAxis( 0.0f ) * 0.5f;
	const idMat3 scaledTo = YawAxis( 30.0f ) * 0.5f;
	CHECK( R_LerpAxis( scaledFrom, scaledTo, 0.25f ).Compare( RowLerp( scaledFrom, scaledTo, 0.25f ), 1e-6f ), "scaled" );
	CHECK( R_LerpAxis( scaledFrom, scaledTo, 0.0f ).Compare( scaledFrom, 1e-6f ), "scaled t = 0" );
	CHECK( R_LerpAxis( scaledFrom, scaledTo, 1.0f ).Compare( scaledTo, 1e-6f ), "scaled t = 1" );

	idMat3 mirrored = YawAxis( 10.0f );
	mirrored[2] = -mirrored[2];
	CHECK( R_LerpAxis( mirrored, YawAxis( 20.0f ), 0.5f ).Compare( RowLerp( mirrored, YawAxis( 20.0f ), 0.5f ), 1e-6f ), "mirrored" );
}

TEST( Interp_PoseJumped ) {
	const idMat3 axis = YawAxis( 45.0f );

	// distance, against r_interpolateSnapDist 64
	CHECK( !R_PoseJumped( vec3_origin, axis, idVec3( 63.0f, 0.0f, 0.0f ), axis, 64.0f ), "63 units" );
	CHECK( R_PoseJumped( vec3_origin, axis, idVec3( 65.0f, 0.0f, 0.0f ), axis, 64.0f ), "65 units" );
	CHECK( !R_PoseJumped( vec3_origin, axis, idVec3( 40.0f, 40.0f, 0.0f ), axis, 64.0f ), "56.6 units diagonal" );
	CHECK( R_PoseJumped( idVec3( 100.0f, 0.0f, 0.0f ), axis, idVec3( 100.0f, 40.0f, 50.0f ), axis, 64.0f ), "64.03 units" );
	CHECK( !R_PoseJumped( vec3_origin, axis, idVec3( 5000.0f, 0.0f, 0.0f ), axis, 0.0f ), "snap distance 0 = no limit" );

	// rotation, more than 90 degrees
	CHECK( !R_PoseJumped( vec3_origin, YawAxis( 0.0f ), vec3_origin, YawAxis( 89.0f ), 64.0f ), "89 degrees" );
	CHECK( R_PoseJumped( vec3_origin, YawAxis( 0.0f ), vec3_origin, YawAxis( 91.0f ), 64.0f ), "91 degrees" );
	CHECK( !R_PoseJumped( vec3_origin, YawAxis( 170.0f ), vec3_origin, YawAxis( -170.0f ), 64.0f ), "20 degrees across 180" );
	CHECK( R_PoseJumped( vec3_origin, mat3_identity, vec3_origin, idAngles( 100.0f, 0.0f, 0.0f ).ToMat3(), 64.0f ), "100 degrees of pitch" );

	// a scaled axis compares by orientation
	CHECK( !R_PoseJumped( vec3_origin, YawAxis( 0.0f ) * 0.5f, vec3_origin, YawAxis( 45.0f ) * 0.5f, 64.0f ), "scaled, 45 degrees" );
	CHECK( R_PoseJumped( vec3_origin, YawAxis( 0.0f ) * 0.5f, vec3_origin, YawAxis( 120.0f ) * 0.5f, 64.0f ), "scaled, 120 degrees" );

	// NaN and infinity never lerp
	const float nan = std::numeric_limits<float>::quiet_NaN();
	CHECK( R_PoseJumped( vec3_origin, axis, idVec3( nan, 0.0f, 0.0f ), axis, 64.0f ), "NaN origin" );
	CHECK( R_PoseJumped( vec3_origin, axis, idVec3( nan, 0.0f, 0.0f ), axis, 0.0f ), "NaN origin, no limit" );
	CHECK( R_PoseJumped( vec3_origin, axis, idVec3( std::numeric_limits<float>::infinity(), 0.0f, 0.0f ), axis, 0.0f ), "infinite origin" );
	idMat3 nanAxis = axis;
	nanAxis[0][0] = nan;
	CHECK( R_PoseJumped( vec3_origin, axis, vec3_origin, nanAxis, 64.0f ), "NaN axis" );

	// no move at all
	CHECK( !R_PoseJumped( vec3_origin, axis, vec3_origin, axis, 64.0f ), "no move" );
	CHECK( Near( R_AxisRotationCos( axis, axis ), 1.0f, 1e-5f ), "cos of no turn" );
}

// a projected light as idLight spawns it; the shader and prelight pointers are only compared
static renderLight_t TestSpotLight() {
	renderLight_t light;
	memset( &light, 0, sizeof( light ) );
	light.axis = YawAxis( 30.0f );
	light.origin = idVec3( 100.0f, -50.0f, 64.0f );
	light.target = idVec3( 0.0f, 0.0f, -256.0f );
	light.right = idVec3( 128.0f, 0.0f, 0.0f );
	light.up = idVec3( 0.0f, 128.0f, 0.0f );
	light.end = light.target;
	light.lightRadius = idVec3( 300.0f, 300.0f, 300.0f );
	light.shaderParms[0] = 1.0f;
	return light;
}

TEST( Interp_LightShapeMatches ) {
	static int materialA, materialB, prelight;
	const idMaterial * shaderA = reinterpret_cast<const idMaterial *>( &materialA );
	const idMaterial * shaderB = reinterpret_cast<const idMaterial *>( &materialB );

	renderLight_t from = TestSpotLight();
	from.shader = shaderA;

	// the pose and what is evaluated per frame lerp
	renderLight_t moved = from;
	moved.origin += idVec3( 12.0f, -3.0f, 1.0f );
	moved.axis = YawAxis( 41.0f );
	moved.shaderParms[0] = 0.25f;
	moved.shaderParms[7] = 3.0f;
	moved.noShadows = true;
	moved.forceShadows = true;
	moved.noSpecular = true;
	moved.lightId = 7;
	moved.suppressLightInViewID = 2;
	moved.viewAttachedID = 3;
	CHECK( R_LightShapeMatches( from, moved, false ), "pose, shader parms, shadow flags, ids" );
	CHECK( R_LightShapeMatches( from, from, false ), "no change" );

	// every change of the volume or the shader snaps
	struct shapeChange_t {
		const char *	name;
		void			( *change )( renderLight_t & light, const idMaterial * other );
	};
	const shapeChange_t changes[] = {
		{ "lightRadius",	[]( renderLight_t & l, const idMaterial * ) { l.lightRadius.y += 1.0f; } },
		{ "lightCenter",	[]( renderLight_t & l, const idMaterial * ) { l.lightCenter.z = 8.0f; } },
		{ "target",			[]( renderLight_t & l, const idMaterial * ) { l.target.x = 16.0f; } },
		{ "right",			[]( renderLight_t & l, const idMaterial * ) { l.right *= 1.5f; } },
		{ "up",				[]( renderLight_t & l, const idMaterial * ) { l.up.x = 1.0f; } },
		{ "start",			[]( renderLight_t & l, const idMaterial * ) { l.start.z = -8.0f; } },
		{ "end",			[]( renderLight_t & l, const idMaterial * ) { l.end.z = -512.0f; } },
		{ "pointLight",		[]( renderLight_t & l, const idMaterial * ) { l.pointLight = true; } },
		{ "parallel",		[]( renderLight_t & l, const idMaterial * ) { l.parallel = true; } },
		{ "shader",			[]( renderLight_t & l, const idMaterial * other ) { l.shader = other; } },
		{ "shader NULL",	[]( renderLight_t & l, const idMaterial * ) { l.shader = nullptr; } },
	};
	for ( const shapeChange_t & c : changes ) {
		renderLight_t changed = moved;
		c.change( changed, shaderB );
		CHECK( !R_LightShapeMatches( from, changed, false ), "%s", c.name );
		CHECK( !R_LightShapeMatches( from, changed, true ), "%s, prelight dropped", c.name );
	}

	// UpdateLightDef drops the prelight of a light that moved, the game keeps passing it
	idRenderModel * prelightModel = reinterpret_cast<idRenderModel *>( &prelight );
	renderLight_t mapLight = from;
	mapLight.prelightModel = prelightModel;
	renderLight_t mapLightMoved = moved;
	mapLightMoved.prelightModel = prelightModel;
	CHECK( R_LightShapeMatches( mapLight, mapLightMoved, false ), "the first move keeps the prelight" );
	CHECK( R_LightShapeMatches( from, mapLightMoved, true ), "later moves: dropped in the def, passed by the game" );
	CHECK( !R_LightShapeMatches( from, mapLightMoved, false ), "a new prelight on a light that never moved" );
}

/*
===============================================================================

	Global operator new and delete: every form must come from Heap.cpp, and only its
	_aligned_malloc blocks padded to 16 pass _aligned_msize, the CRT's new doesn't.

===============================================================================
*/

struct alignas( 64 ) heapAligned64_t {
	float	v[16];
};

struct ALIGNTYPE128 heapAligned128_t {
	float	v[4];
};

static int heapNewHandlerCalls = 0;

static void HeapTestNewHandler() {
	heapNewHandlerCalls++;
	std::set_new_handler( nullptr );	// nothing to free, the next failure throws
}

// true when ptr is an _aligned_malloc block of exactly paddedSize bytes, aligned to alignment
static bool IsHeapBlock( const void *ptr, size_t paddedSize, size_t alignment ) {
	return ptr != nullptr && ( reinterpret_cast<uintptr_t>( ptr ) & ( alignment - 1 ) ) == 0
		&& _aligned_msize( const_cast<void *>( ptr ), alignment, 0 ) == paddedSize;
}

template< typename func_t >
static bool ThrowsBadAlloc( func_t func ) {
	try {
		func();
	} catch ( const std::bad_alloc & ) {
		return true;
	}
	return false;
}

TEST( Heap_NewDelete ) {
	// scalar, array, nothrow and tagged forms: 16 byte aligned, size padded to 16
	{
		int *scalar = new int;
		CHECK( IsHeapBlock( scalar, 16, 16 ), "new int" );
		delete scalar;

		char *array = new char[17];
		CHECK( IsHeapBlock( array, 32, 16 ), "new char[17]" );
		delete[] array;

		int *nothrowScalar = new ( std::nothrow ) int;
		CHECK( IsHeapBlock( nothrowScalar, 16, 16 ), "new ( std::nothrow ) int" );
		delete nothrowScalar;

		char *nothrowArray = new ( std::nothrow ) char[33];
		CHECK( IsHeapBlock( nothrowArray, 48, 16 ), "new ( std::nothrow ) char[33]" );
		delete[] nothrowArray;

		idVec3 *tagged = new ( TAG_TEMP ) idVec3;
		CHECK( IsHeapBlock( tagged, 16, 16 ), "new ( TAG_TEMP ) idVec3" );
		delete tagged;

		float *taggedArray = new ( TAG_TEMP ) float[5];
		CHECK( IsHeapBlock( taggedArray, 32, 16 ), "new ( TAG_TEMP ) float[5]" );
		delete[] taggedArray;
	}

	// over-aligned types take the std::align_val_t forms, which must honor alignments above 16
	{
		heapAligned64_t *a64 = new heapAligned64_t;
		CHECK( IsHeapBlock( a64, 64, 64 ), "new of an alignas( 64 ) type" );
		delete a64;

		heapAligned64_t *a64Array = new heapAligned64_t[3];
		CHECK( IsHeapBlock( a64Array, 192, 64 ), "new[] of an alignas( 64 ) type" );
		delete[] a64Array;

		heapAligned128_t *a128 = new ( std::nothrow ) heapAligned128_t;
		CHECK( IsHeapBlock( a128, 128, 128 ), "nothrow new of an ALIGNTYPE128 type" );
		delete a128;
	}

	// zero bytes: a distinct non-null pointer every time, also from the tagged form
	{
		void *a = ::operator new( 0 );
		void *b = ::operator new( 0 );
		CHECK( a != nullptr && b != nullptr && a != b, "operator new( 0 ) %p %p", a, b );
		::operator delete( a );
		::operator delete( b );

		char *tagged = new ( TAG_TEMP ) char[0];
		CHECK( tagged != nullptr, "new ( TAG_TEMP ) char[0] returned nullptr" );
		delete[] tagged;
	}

	// failure throws std::bad_alloc, never returns nullptr or a block smaller than asked for
	{
		// new[] passes SIZE_MAX when the element count overflows; padding it to 16 must not wrap around to 0
		volatile size_t hugeCount = SIZE_MAX / 2;
		CHECK( ThrowsBadAlloc( [&] { delete[] new int[hugeCount]; } ), "new int[SIZE_MAX / 2] didn't throw" );
		CHECK( ThrowsBadAlloc( [] { ::operator delete( ::operator new( SIZE_MAX ) ); } ), "operator new( SIZE_MAX ) didn't throw" );
		CHECK( ThrowsBadAlloc( [] { ::operator delete( ::operator new( SIZE_MAX - 3, TAG_TEMP ) ); } ), "tagged operator new( SIZE_MAX - 3 ) didn't throw" );
		// a real out of memory: no 3.75 GB hole in a 32-bit address space
		CHECK( ThrowsBadAlloc( [] { ::operator delete[]( ::operator new[]( 0xF0000000u ) ); } ), "operator new[]( 3.75 GB ) didn't throw" );
		CHECK( ThrowsBadAlloc( [] { ::operator delete( ::operator new( SIZE_MAX - 3, std::align_val_t{ 64 } ), std::align_val_t{ 64 } ); } ), "aligned operator new( SIZE_MAX - 3 ) didn't throw" );

		CHECK( ::operator new( SIZE_MAX, std::nothrow ) == nullptr, "nothrow operator new( SIZE_MAX )" );
		CHECK( ::operator new[]( 0xF0000000u, std::nothrow ) == nullptr, "nothrow operator new[]( 3.75 GB )" );
		CHECK( ::operator new( SIZE_MAX, std::align_val_t{ 128 }, std::nothrow ) == nullptr, "nothrow aligned operator new( SIZE_MAX )" );
		CHECK( new ( std::nothrow ) int[hugeCount] == nullptr, "new ( std::nothrow ) int[SIZE_MAX / 2]" );

		// the new handler runs before the exception; this one uninstalls itself
		heapNewHandlerCalls = 0;
		std::set_new_handler( HeapTestNewHandler );
		CHECK( ThrowsBadAlloc( [] { ::operator delete( ::operator new( SIZE_MAX ) ); } ), "operator new( SIZE_MAX ) with a new handler didn't throw" );
		CHECK( heapNewHandlerCalls == 1, "new handler called %d times", heapNewHandlerCalls );
		CHECK( std::get_new_handler() == nullptr, "new handler still installed" );
		std::set_new_handler( nullptr );
	}
}

/*
===============================================================================

	main

===============================================================================
*/

int main( int argc, char **argv ) {
	const char *filter = ( argc > 1 && idStr::Icmp( argv[1], "all" ) != 0 ) ? argv[1] : NULL;
	if ( getenv( "IDLIB_TESTS_VERBOSE" ) != NULL ) {
		MAX_FAILURES_PER_TEST = 1 << 30;
	}

	idLib::Init();

	genericProcessor = new idSIMD_Generic;
	genericProcessor->cpuid = CPUID_GENERIC;
	sseProcessor = new idSIMD_SSE;
	sseProcessor->cpuid = (cpuid_t)( CPUID_MMX | CPUID_SSE | CPUID_SSE2 );
	SIMDProcessor = sseProcessor;		// same as the game on any x86 CPU of the last 20 years

	// the game threads run with flush-to-zero and denormals-are-zero
	if ( argc > 2 && idStr::Icmp( argv[2], "noftz" ) == 0 ) {
		printf( "denormals enabled\n" );
	} else {
		_mm_setcsr( _mm_getcsr() | 0x8040 );
	}

	printf( "idLib tests, SIMD processor: %s\n", SIMDProcessor->GetName() );

	int numRun = 0;
	int numFailedTests = 0;
	for ( int i = 0; i < numTestCases; i++ ) {
		if ( filter != NULL && strstr( testCases[i].name, filter ) == NULL ) {
			continue;
		}
		currentTest = testCases[i].name;
		numFailuresInTest = 0;
		printf( "[ RUN  ] %s\n", currentTest );
		testCases[i].func();
		if ( numFailuresInTest > MAX_FAILURES_PER_TEST ) {
			printf( "  ... %d more failures\n", numFailuresInTest - MAX_FAILURES_PER_TEST );
		}
		printf( "[ %s ] %s\n", numFailuresInTest == 0 ? " OK " : "FAIL", currentTest );
		numRun++;
		if ( numFailuresInTest != 0 ) {
			numFailedTests++;
		}
	}

	printf( "\n%d tests, %d checks, %d failed checks, %d failed tests\n", numRun, numChecks, numFailures, numFailedTests );
	return ( numFailedTests == 0 ) ? 0 : 1;
}
