// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "../idlib/precompiled.h"

#include "Game_local.h"

static const char * fxSystem_SnapshotName = "_FxSystem_Snapshot_";

static constexpr float FX_REST_SPEED = 20.0f;		// a spark slower than this after a bounce lies still
static constexpr float FX_SPARK_STREAK = 0.035f;	// seconds of flight a spark's streak covers
static constexpr float FX_MARK_LIFT = 0.4f;			// a mark floats this far off its surface

/*
================
idFxSystem::idFxSystem
================
*/
idFxSystem::idFxSystem() {
	initialized = false;
	memset( &renderEntity, 0, sizeof( renderEntity ) );
	renderEntityHandle = -1;
	material = nullptr;
	simTime = 0;
	builtViewTime = -1;
}

/*
================
idFxSystem::Init
================
*/
void idFxSystem::Init() {
	if ( initialized ) {
		Shutdown();
	}
	particles.Clear();
	random.SetSeed( 0x5eed );
	simTime = gameLocal.slow.time;
	builtViewTime = -1;
	material = declManager->FindMaterial( "fx/emissive" );
	tests.Clear();
	testLightShader = declManager->FindMaterial( "lights/fx_energy" );

	memset( &renderEntity, 0, sizeof( renderEntity ) );
	renderEntity.axis = mat3_identity;
	renderEntity.shaderParms[ SHADERPARM_RED ] = 1.0f;
	renderEntity.shaderParms[ SHADERPARM_GREEN ] = 1.0f;
	renderEntity.shaderParms[ SHADERPARM_BLUE ] = 1.0f;
	renderEntity.shaderParms[ SHADERPARM_ALPHA ] = 1.0f;
	renderEntity.hModel = renderModelManager->AllocModel();
	renderEntity.hModel->InitEmpty( fxSystem_SnapshotName );
	renderEntity.noShadow = 1;
	// present in every area of the world, as the smoke is
	renderEntity.bounds.Clear();
	renderEntity.bounds.AddPoint( idVec3( -100000.0f, -100000.0f, -100000.0f ) );
	renderEntity.bounds.AddPoint( idVec3( 100000.0f, 100000.0f, 100000.0f ) );
	renderEntity.callback = idFxSystem::ModelCallback;
	renderEntityHandle = gameRenderWorld->AddEntityDef( &renderEntity );

	initialized = true;

	// every effect now, at the level load: the game thread may not load a decl
	if ( cvarSystem->GetCVarBool( "com_enhancedEffects" ) ) {
		for ( int i = 0; i < declManager->GetNumDecls( DECL_ENTITYDEF ); i++ ) {
			const idDecl * decl = declManager->DeclByIndex( DECL_ENTITYDEF, i, false );
			if ( idStr::Icmpn( decl->GetName(), "fx_", 3 ) == 0 ) {
				LoadDef( decl->GetName() );
			}
		}
	}
}

/*
================
idFxSystem::Shutdown
================
*/
void idFxSystem::Shutdown() {
	if ( renderEntityHandle != -1 ) {
		gameRenderWorld->FreeEntityDef( renderEntityHandle );
		renderEntityHandle = -1;
	}
	if ( initialized ) {
		FreeTests();
	}
	if ( renderEntity.hModel != nullptr ) {
		renderModelManager->FreeModel( renderEntity.hModel );
		renderEntity.hModel = nullptr;
	}
	particles.Clear();
	defs.DeleteContents( true );
	initialized = false;
}

/*
================
idFxSystem::FindDef
================
*/
const fxDef_t * idFxSystem::FindDef( const char * name ) {
	if ( !initialized || name == nullptr || name[0] == '\0' || !cvarSystem->GetCVarBool( "com_enhancedEffects" ) ) {
		return nullptr;
	}
	for ( int i = 0; i < defs.Num(); i++ ) {
		if ( defs[i]->name.Icmp( name ) == 0 ) {
			return defs[i];
		}
	}
	gameLocal.Warning( "idFxSystem: no effect '%s'", name );
	return nullptr;
}

/*
================
idFxSystem::LoadDef
================
*/
void idFxSystem::LoadDef( const char * name ) {
	const idDict * dict = gameLocal.FindEntityDefDict( name, false );
	if ( dict == nullptr ) {
		return;
	}

	static const char * kindNames[] = { "glow", "spark", "mark", "flash", "orb" };
	static_assert( sizeof( kindNames ) / sizeof( kindNames[0] ) == static_cast<int>( fxKind_t::COUNT ) );

	fxDef_t * def = new ( TAG_PARTICLE ) fxDef_t;
	def->name = name;
	def->lightColor = dict->GetVector( "light_color", "0 0 0" );
	def->lightRadius = dict->GetFloat( "light_radius", "0" );
	for ( int n = 1; ; n++ ) {
		const idStr prefix = va( "e%d_", n );
		const char * kindName = dict->GetString( prefix + "kind" );
		if ( kindName[0] == '\0' ) {
			break;
		}
		fxEmitter_t emitter;
		emitter.kind = fxKind_t::GLOW;
		for ( int k = 0; k < static_cast<int>( fxKind_t::COUNT ); k++ ) {
			if ( idStr::Icmp( kindName, kindNames[k] ) == 0 ) {
				emitter.kind = static_cast<fxKind_t>( k );
			}
		}
		emitter.count = dict->GetInt( prefix + "count", "1" );
		emitter.spacing = dict->GetFloat( prefix + "spacing", "0" );
		emitter.rate = dict->GetFloat( prefix + "rate", "0" );
		emitter.speedMin = dict->GetFloat( prefix + "speedMin", "0" );
		emitter.speedMax = dict->GetFloat( prefix + "speedMax", va( "%f", emitter.speedMin ) );
		emitter.cone = dict->GetFloat( prefix + "cone", "0" );
		emitter.inherit = dict->GetFloat( prefix + "inherit", "0" );
		emitter.lifeMin = dict->GetFloat( prefix + "lifeMin", "0.5" );
		emitter.lifeMax = dict->GetFloat( prefix + "lifeMax", va( "%f", emitter.lifeMin ) );
		emitter.size = dict->GetFloat( prefix + "size", "4" );
		emitter.sizeEnd = dict->GetFloat( prefix + "sizeEnd", va( "%f", emitter.size ) );
		emitter.gravity = dict->GetFloat( prefix + "gravity", "0" );
		emitter.drag = dict->GetFloat( prefix + "drag", "0" );
		emitter.bounce = dict->GetFloat( prefix + "bounce", "-1" );
		emitter.heat = dict->GetFloat( prefix + "heat", "0" );
		emitter.intensity = dict->GetFloat( prefix + "intensity", "1" );
		emitter.fadeIn = dict->GetFloat( prefix + "fadeIn", "0" );
		emitter.fadeDepth = dict->GetFloat( prefix + "fadeDepth", "0" );
		emitter.offset = dict->GetFloat( prefix + "offset", "0" );
		emitter.spread = dict->GetFloat( prefix + "spread", "0" );
		emitter.vary = dict->GetFloat( prefix + "vary", "0" );
		emitter.color = dict->GetVector( prefix + "color", "1 1 1" );
		def->emitters.Append( emitter );
	}
	defs.Append( def );
}

/*
================
idFxSystem::Emit
================
*/
idFxSystem::particle_t * idFxSystem::Emit( const fxEmitter_t & emitter, const idVec3 & origin, const idVec3 & axis, const idVec3 & baseVelocity, int startTime ) {
	if ( particles.Num() >= MAX_PARTICLES ) {
		return nullptr;
	}
	const idMat3 frame = axis.ToMat3();
	const float angle = DEG2RAD( emitter.cone ) * random.RandomFloat();
	const float turn = idMath::TWO_PI * random.RandomFloat();
	const idVec3 dir = frame[0] * idMath::Cos( angle ) + ( frame[1] * idMath::Cos( turn ) + frame[2] * idMath::Sin( turn ) ) * idMath::Sin( angle );
	const float speed = emitter.speedMin + ( emitter.speedMax - emitter.speedMin ) * random.RandomFloat();
	const float life = emitter.lifeMin + ( emitter.lifeMax - emitter.lifeMin ) * random.RandomFloat();
	// a ball, not a cube: the corners come back to its shell
	idVec3 jitter( random.CRandomFloat(), random.CRandomFloat(), random.CRandomFloat() );
	if ( jitter.LengthSqr() > 1.0f ) {
		jitter.Normalize();
	}

	particle_t & p = *particles.Alloc();
	p.origin = origin + axis * emitter.offset + jitter * emitter.spread;
	p.velocity = baseVelocity + dir * speed;
	p.normal = axis;
	p.startTime = startTime;
	p.endTime = startTime + Max( 1, idMath::Ftoi( life * 1000.0f ) );
	p.size = emitter.size;
	p.sizeEnd = emitter.sizeEnd;
	p.gravity = emitter.gravity;
	p.drag = emitter.drag;
	p.bounce = emitter.bounce;
	p.fadeIn = emitter.fadeIn;
	for ( int i = 0; i < 3; i++ ) {
		p.color[i] = static_cast<byte>( idMath::ClampInt( 0, 255, idMath::Ftoi( emitter.color[i] * 255.0f + 0.5f ) ) );
	}
	p.kind = static_cast<byte>( emitter.kind );
	p.seed = static_cast<byte>( random.RandomInt( 256 ) );
	p.heat = static_cast<byte>( idMath::ClampInt( 0, 255, idMath::Ftoi( emitter.heat * 255.0f + 0.5f ) ) );
	// fx_emissive.hlsl: the intensity is 2^( b / 16 - 4 ), the fade depth b / 255 * 64 units
	// vary: a few bright ones among many dim ones, the mean stays
	const float bright = random.RandomFloat();
	const float intensity = emitter.intensity * ( 1.0f - emitter.vary + emitter.vary * 3.0f * bright * bright );
	const float exponent = ( idMath::Log( Max( intensity, 0.0625f ) ) / idMath::Log( 2.0f ) + 4.0f ) * 16.0f;
	p.intensity = static_cast<byte>( idMath::ClampInt( 0, 255, idMath::Ftoi( exponent + 0.5f ) ) );
	p.fadeDepth = static_cast<byte>( idMath::ClampInt( 0, 255, idMath::Ftoi( emitter.fadeDepth * ( 255.0f / 64.0f ) + 0.5f ) ) );
	p.oneTic = false;
	return &p;
}

/*
================
idFxSystem::Burst
================
*/
void idFxSystem::Burst( const fxDef_t * def, const idVec3 & origin, const idVec3 & axis ) {
	if ( def == nullptr || !initialized ) {
		return;
	}
	for ( int e = 0; e < def->emitters.Num(); e++ ) {
		const fxEmitter_t & emitter = def->emitters[e];
		if ( emitter.kind == fxKind_t::MARK ) {
			// only on a solid surface right behind the burst
			trace_t hit;
			gameLocal.clip.TracePoint( hit, origin + axis * 4.0f, origin - axis * 12.0f, CONTENTS_SOLID, nullptr );
			if ( hit.fraction >= 1.0f ) {
				continue;
			}
			particle_t * mark = Emit( emitter, hit.endpos, hit.c.normal, vec3_origin, gameLocal.slow.time );
			if ( mark != nullptr ) {
				mark->origin = hit.endpos;
				mark->velocity.Zero();
			}
			continue;
		}
		for ( int i = 0; i < emitter.count; i++ ) {
			Emit( emitter, origin, axis, vec3_origin, gameLocal.slow.time );
		}
	}
}

/*
================
idFxSystem::Trail
================
*/
void idFxSystem::Trail( const fxDef_t * def, const idVec3 & from, const idVec3 & to, const idVec3 & velocity, float & travelled, int seed ) {
	if ( def == nullptr || !initialized ) {
		return;
	}
	idVec3 back = -velocity;
	if ( back.Normalize() == 0.0f ) {
		back.Set( 0.0f, 0.0f, 1.0f );
	}
	const float length = ( to - from ).Length();
	const int ticMsec = gameLocal.slow.time - gameLocal.slow.previousTime;
	for ( int e = 0; e < def->emitters.Num(); e++ ) {
		const fxEmitter_t & emitter = def->emitters[e];
		// a particle born at f of the tic starts that much of its own flight back: drawn, it leaves the projectile
		const idVec3 base = velocity * emitter.inherit;
		if ( emitter.rate > 0.0f ) {
			const double perMsec = emitter.rate / 1000.0;
			const int count = static_cast<int>( perMsec * gameLocal.slow.time ) - static_cast<int>( perMsec * gameLocal.slow.previousTime );
			for ( int i = 0; i < count; i++ ) {
				const float f = ( i + random.RandomFloat() ) / count;
				Emit( emitter, from + ( to - from ) * f - base * MS2SEC( f * ticMsec ), back, base, gameLocal.slow.time + idMath::Ftoi( f * ticMsec ) );
			}
			continue;
		}
		if ( emitter.spacing <= 0.0f ) {
			// it rides the projectile, which is drawn between its last pose and this one while the view's time
			// runs on from this tic (r_interpolate); gone at the next tic, where the projectile emits it again
			particle_t * p = Emit( emitter, from, back, vec3_origin, gameLocal.slow.time );
			if ( p != nullptr ) {
				p->velocity = ( ticMsec > 0 ) ? ( to - from ) * ( 1000.0f / ticMsec ) : velocity;
				p->oneTic = true;
				p->seed = static_cast<byte>( seed );
			}
			continue;
		}
		if ( length <= 0.0f ) {
			continue;
		}
		// one at every multiple of the spacing the flight crossed this tic, born when the drawn projectile passes it
		const float first = ( idMath::Floor( travelled / emitter.spacing ) + 1.0f ) * emitter.spacing;
		for ( float at = first; at <= travelled + length; at += emitter.spacing ) {
			const float f = ( at - travelled ) / length;
			const int startTime = gameLocal.slow.time + idMath::Ftoi( f * ticMsec );
			Emit( emitter, from + ( to - from ) * f - base * MS2SEC( f * ticMsec ), back, base, startTime );
		}
	}
	travelled += length;
}

/*
================
idFxSystem::Test
================
*/
void idFxSystem::Test( const char * name, const idVec3 & origin ) {
	if ( !initialized ) {
		return;
	}
	const fxDef_t * def = FindDef( name );
	if ( def == nullptr || tests.Num() == tests.Max() ) {
		FreeTests();
		return;
	}
	test_t & test = *tests.Alloc();
	test.def = def;
	test.origin = origin;
	test.travelled = 0.0f;
	test.lightHandle = -1;
	if ( def->lightRadius > 0.0f ) {
		memset( &test.light, 0, sizeof( test.light ) );
		test.light.axis = mat3_identity;
		test.light.origin = origin;
		test.light.pointLight = true;
		test.light.lightRadius.Set( def->lightRadius, def->lightRadius, def->lightRadius );
		test.light.shader = testLightShader;
		test.light.shaderParms[ SHADERPARM_ALPHA ] = 1.0f;
		test.lightHandle = gameRenderWorld->AddLightDef( &test.light );
	}
}

/*
================
idFxSystem::FreeTests
================
*/
void idFxSystem::FreeTests() {
	for ( int i = 0; i < tests.Num(); i++ ) {
		if ( tests[i].lightHandle != -1 ) {
			gameRenderWorld->FreeLightDef( tests[i].lightHandle );
		}
	}
	tests.Clear();
}

/*
================
idFxSystem::RunFrame
================
*/
void idFxSystem::RunFrame() {
	if ( !initialized ) {
		return;
	}
	const int time = gameLocal.slow.time;
	const float dt = MS2SEC( time - simTime );
	simTime = time;

	int traces = 0;
	for ( int i = particles.Num() - 1; i >= 0; i-- ) {
		particle_t & p = particles[i];
		if ( p.oneTic || time >= p.endTime ) {
			p = particles[ particles.Num() - 1 ];
			particles.SetNum( particles.Num() - 1 );
			continue;
		}
		if ( dt <= 0.0f || p.velocity.LengthSqr() == 0.0f ) {
			continue;
		}
		p.velocity.z -= p.gravity * dt;
		p.velocity *= Max( 0.0f, 1.0f - p.drag * dt );
		const idVec3 end = p.origin + p.velocity * dt;
		if ( p.bounce < 0.0f || traces >= MAX_TRACES_PER_TIC ) {
			p.origin = end;
			continue;
		}
		// solid things alone (much of a level is static entities, not the world model): no bodies, no render models
		traces++;
		trace_t hit;
		gameLocal.clip.TracePoint( hit, p.origin, end, CONTENTS_SOLID, nullptr );
		if ( hit.fraction >= 1.0f ) {
			p.origin = end;
			continue;
		}
		p.origin = hit.endpos + hit.c.normal * 0.25f;
		p.velocity -= 2.0f * ( p.velocity * hit.c.normal ) * hit.c.normal;
		p.velocity *= p.bounce;
		if ( p.velocity.LengthSqr() < FX_REST_SPEED * FX_REST_SPEED ) {
			p.velocity.Zero();
		}
	}

	for ( int i = 0; i < tests.Num(); i++ ) {
		test_t & test = tests[i];
		Trail( test.def, test.origin, test.origin, vec3_origin, test.travelled, 77 + i * 31 );
		if ( test.lightHandle != -1 ) {
			const float flicker = 0.75f + 0.5f * random.RandomFloat();
			test.light.shaderParms[ SHADERPARM_RED ] = test.def->lightColor.x * flicker;
			test.light.shaderParms[ SHADERPARM_GREEN ] = test.def->lightColor.y * flicker;
			test.light.shaderParms[ SHADERPARM_BLUE ] = test.def->lightColor.z * flicker;
			gameRenderWorld->UpdateLightDef( test.lightHandle, &test.light );
		}
	}
}

/*
================
idFxSystem::UpdateRenderEntity

Quads for the view's time: it leads the tic, and a particle moves on along its velocity in between.
================
*/
bool idFxSystem::UpdateRenderEntity( renderEntity_s * entity, const renderView_t * renderView ) {
	if ( renderView == nullptr ) {
		// a trace: an empty model
		entity->hModel->InitEmpty( fxSystem_SnapshotName );
		return false;
	}
	const int viewTime = renderView->time[ TIME_GROUP1 ];
	if ( viewTime == builtViewTime && !renderView->forceUpdate ) {
		return false;
	}
	builtViewTime = viewTime;
	entity->hModel->InitEmpty( fxSystem_SnapshotName );
	if ( particles.Num() == 0 ) {
		return true;
	}

	srfTriangles_t * tri = entity->hModel->AllocSurfaceTriangles( particles.Num() * 4, particles.Num() * 6 );
	tri->bounds[0].Set( -99999.0f, -99999.0f, -99999.0f );
	tri->bounds[1].Set( 99999.0f, 99999.0f, 99999.0f );

	// fx_emissive.hlsl: the view's time in 1/256 s, 16 bits
	const int clock = ( static_cast<int64>( viewTime ) * 256 / 1000 ) & 0xFFFF;
	const float lead = idMath::ClampFloat( -0.02f, 0.05f, MS2SEC( viewTime - simTime ) );
	const idVec3 & viewLeft = renderView->viewaxis[1];
	const idVec3 & viewUp = renderView->viewaxis[2];
	int numVerts = 0;
	for ( int i = 0; i < particles.Num(); i++ ) {
		const particle_t & p = particles[i];
		float age = 0.0f;
		if ( !p.oneTic ) {
			age = static_cast<float>( viewTime - p.startTime ) / static_cast<float>( p.endTime - p.startTime );
			if ( age < 0.0f || age >= 1.0f ) {
				continue;
			}
		}
		const fxKind_t kind = static_cast<fxKind_t>( p.kind );
		const float size = p.size + ( p.sizeEnd - p.size ) * age;
		const idVec3 origin = p.origin + p.velocity * lead;

		// the envelope: marks and sparks cool in the program, the light fades here
		float envelope = ( p.fadeIn > 0.0f ) ? Min( 1.0f, age / p.fadeIn ) : 1.0f;
		if ( kind == fxKind_t::GLOW || kind == fxKind_t::FLASH || kind == fxKind_t::ORB ) {
			envelope *= ( 1.0f - age ) * ( 1.0f - age );
		} else if ( kind == fxKind_t::SPARK ) {
			envelope *= 1.0f - age * age * age * age;
		}

		idVec3 corner[4];
		if ( kind == fxKind_t::SPARK ) {
			// a streak back along the flight, turned to face the eye
			const float speed = p.velocity.Length();
			const idVec3 dir = ( speed > 0.0f ) ? p.velocity * ( 1.0f / speed ) : viewLeft;
			const idVec3 tail = origin - dir * Max( size * 2.0f, speed * FX_SPARK_STREAK );
			idVec3 side = dir.Cross( renderView->vieworg - origin );
			if ( side.Normalize() == 0.0f ) {
				side = viewUp;
			}
			side *= size * 0.5f;
			corner[0] = tail - side;
			corner[1] = origin - side;
			corner[2] = tail + side;
			corner[3] = origin + side;
		} else {
			idVec3 left = viewLeft;
			idVec3 up = viewUp;
			idVec3 center = origin;
			if ( kind == fxKind_t::MARK ) {
				p.normal.NormalVectors( left, up );
				center += p.normal * FX_MARK_LIFT;
			}
			left *= size;
			up *= size;
			corner[0] = center + left + up;
			corner[1] = center - left + up;
			corner[2] = center + left - up;
			corner[3] = center - left - up;
		}

		idDrawVert * verts = tri->verts + numVerts;
		for ( int j = 0; j < 4; j++ ) {
			idDrawVert & v = verts[j];
			v.Clear();
			v.xyz = corner[j];
			v.SetTexCoord( static_cast<float>( j & 1 ), static_cast<float>( j >> 1 ) );
			v.color[0] = p.color[0];
			v.color[1] = p.color[1];
			v.color[2] = p.color[2];
			v.color[3] = static_cast<byte>( idMath::Ftoi( idMath::ClampFloat( 0.0f, 1.0f, envelope ) * 255.0f + 0.5f ) );
			v.normal[0] = static_cast<byte>( idMath::Ftoi( age * 255.0f ) );
			v.normal[1] = p.seed;
			v.normal[2] = p.heat;
			v.normal[3] = p.kind;
			v.tangent[0] = p.intensity;
			v.tangent[1] = p.fadeDepth;
			v.tangent[2] = static_cast<byte>( clock >> 8 );
			v.tangent[3] = static_cast<byte>( clock & 255 );
		}
		numVerts += 4;
	}

	if ( numVerts == 0 ) {
		entity->hModel->FreeSurfaceTriangles( tri );
		return true;
	}
	int numIndexes = 0;
	for ( int i = 0; i < numVerts; i += 4 ) {
		tri->indexes[ numIndexes + 0 ] = static_cast<triIndex_t>( i );
		tri->indexes[ numIndexes + 1 ] = static_cast<triIndex_t>( i + 2 );
		tri->indexes[ numIndexes + 2 ] = static_cast<triIndex_t>( i + 3 );
		tri->indexes[ numIndexes + 3 ] = static_cast<triIndex_t>( i );
		tri->indexes[ numIndexes + 4 ] = static_cast<triIndex_t>( i + 3 );
		tri->indexes[ numIndexes + 5 ] = static_cast<triIndex_t>( i + 1 );
		numIndexes += 6;
	}
	tri->numVerts = numVerts;
	tri->numIndexes = numIndexes;

	modelSurface_t surf;
	surf.geometry = tri;
	surf.shader = material;
	surf.id = 0;
	entity->hModel->AddSurface( surf );
	return true;
}

/*
================
idFxSystem::ModelCallback
================
*/
bool idFxSystem::ModelCallback( renderEntity_s * renderEntity, const renderView_t * renderView ) {
	if ( gameLocal.fxSystem == nullptr ) {
		return false;
	}
	return gameLocal.fxSystem->UpdateRenderEntity( renderEntity, renderView );
}
