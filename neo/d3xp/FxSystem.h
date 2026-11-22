// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#ifndef __FXSYSTEM_H__
#define __FXSYSTEM_H__

/*
===============================================================================

	The effect system (com_enhancedEffects): particles with a state of their own, which the parametric
	particles of idDeclParticle lack. A spark has a velocity, falls, bounces off the world and cools; a
	glow follows its projectile between tics; a mark stays on the surface it burnt.

	An effect is an entityDef "fx_*" read as a dictionary: emitters e1, e2, ... with the keys
	e<n>_kind (glow, spark, mark, flash) and the fxEmitter_t fields. It is drawn by the fx_emissive
	program from the bytes of each vertex.

	It never draws the game's random numbers, spawns no entity and is not saved, so the game runs the
	same with and without it. One world render entity presents it, like idSmokeParticles.

===============================================================================
*/

enum class fxKind_t : byte {
	GLOW,
	SPARK,
	MARK,
	FLASH,
	ORB,
	COUNT
};

struct fxEmitter_t {
	fxKind_t	kind;
	int			count;				// per burst
	float		spacing;			// a trail: one every so many units of flight; 0 = one that follows the projectile
	float		rate;				// a trail: so many a second whatever the speed
	float		speedMin, speedMax;
	float		cone;				// degrees around the effect's axis
	float		inherit;			// the fraction of the projectile's velocity a trail particle keeps
	float		lifeMin, lifeMax;	// seconds
	float		size, sizeEnd;
	float		gravity, drag;
	float		bounce;				// the speed kept across a bounce off the world; negative = no collision
	float		heat;				// 0 = the colour, above it the colour of a glowing body cooling from that heat;
									// an orb's: how far its arcs reach, 0..1
	float		intensity;			// times the colour: radiance relative to a lit wall
	float		fadeIn;				// the fraction of the life it takes to appear
	float		fadeDepth;			// units over which it fades at geometry, 0 = none
	float		offset;				// along the axis
	float		spread;				// the radius of the random start
	float		vary;				// 0..1: how unlike in brightness the particles are
	idVec3		color;
};

struct fxDef_t {
	idStr					name;
	idList<fxEmitter_t>		emitters;
	idVec3					lightColor;		// fxTest's light; a projectile has its own
	float					lightRadius;
};

class idFxSystem {
public:
							idFxSystem();

	void					Init();
	void					Shutdown();

	// NULL when the effects are off or the def does not exist
	const fxDef_t *			FindDef( const char * name );

	// once a tic, before the entities think
	void					RunFrame();

	// an explosion, an impact or a shot at origin along axis (a surface's normal)
	void					Burst( const fxDef_t * def, const idVec3 & origin, const idVec3 & axis );
	// a tic of a projectile's flight; travelled is its state, the distance flown so far
	// seed tells its followers from another projectile's
	void					Trail( const fxDef_t * def, const idVec3 & from, const idVec3 & to, const idVec3 & velocity, float & travelled, int seed );
	// fxTest: the trail of a projectile that hangs at origin, with a light; no def ends them all
	void					Test( const char * name, const idVec3 & origin );

private:
	struct particle_t {
		idVec3				origin;
		idVec3				velocity;
		idVec3				normal;			// a mark's surface
		int					startTime;
		int					endTime;
		float				size, sizeEnd;
		float				gravity, drag, bounce;
		float				fadeIn;
		byte				color[3];
		byte				kind;
		byte				seed;
		byte				heat;
		byte				intensity;
		byte				fadeDepth;
		bool				oneTic;			// follows its projectile: emitted again every tic
	};

	static constexpr int	MAX_PARTICLES = 8192;		// 4 vertices each: the most 16-bit indexes reach
	static constexpr int	MAX_TRACES_PER_TIC = 256;

	bool					initialized;
	renderEntity_t			renderEntity;
	int						renderEntityHandle;
	const idMaterial *		material;
	idList<fxDef_t *>		defs;
	idStaticList<particle_t, MAX_PARTICLES>	particles;
	idRandom				random;			// our own: the game's stream stays as it is
	int						simTime;		// the tic the particles were stepped to
	int						builtViewTime;
	struct test_t {
		const fxDef_t *		def;
		idVec3				origin;
		float				travelled;
		renderLight_t		light;
		int					lightHandle;
	};
	idStaticList<test_t, 4>	tests;
	const idMaterial *		testLightShader;

	void					LoadDef( const char * name );
	void					FreeTests();
	particle_t *			Emit( const fxEmitter_t & emitter, const idVec3 & origin, const idVec3 & axis, const idVec3 & baseVelocity, int startTime );
	bool					UpdateRenderEntity( renderEntity_s * entity, const renderView_t * renderView );
	static bool				ModelCallback( renderEntity_s * renderEntity, const renderView_t * renderView );
};

#endif /* !__FXSYSTEM_H__ */
