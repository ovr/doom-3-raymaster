// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

/*
	What the main view's G-buffer (R14) and its ray traced passes do: the r_gbuffer* and r_rt* cvars, read by
	the front end into viewDef_t (R_DefaultRayTracingSettings), so the back end and the backends read no cvar.
	Nothing is written while no consumer asks for it: the frames are then those without the G-buffer.
*/

enum class gbufferDebug_t : uint8 {
	NONE,
	NORMAL,		// the world normal as colour, 0.5 + 0.5 n
	F0,			// the reflectance at normal incidence
	ROUGHNESS,
	MOTION,		// direction as hue, length as brightness (8 pixels = full)
	ALBEDO,		// the diffuse map times its stage colour
	COUNT
};

// r_rtDebug: a ray per pixel of the main view against the GPU scene's acceleration structures.
enum class rtDebug_t : uint8 {
	NONE,
	INSTANCES,	// a colour per instance (its first geometry record, stable while its slot lives), shaded by the facing
	PRIMARY,	// against the depth: grey agrees, red misses a drawn surface, blue hits in front of one, yellow a back face
	NORMAL,		// the hit's interpolated vertex normal in world space, 0.5 + 0.5 n
	ALBEDO,		// the hit's diffuse texture times its stage colour, as stored (gamma)
	REFLECTION,	// the mirror reflections alone, in place of the lights (the passes after them still draw)
	SHADOWS,	// what the traced shadows leave of the traced lights, in place of the lights; blue where none reaches
	HISTORY,	// how many frames the traced shadows' history holds per pixel, black none, white all it may
	COUNT
};

// The main view as its rays need it: GL's clip space of shared code, the eye, the angle of a pixel; and its
// place in the eye's history (R18), which a pass reads only from the frame before of the same epoch.
struct rtView_t {
	float				clipToWorld[4][4] = {};	// rows of the inverse of the view's MVP
	float				worldToClip[4][4] = {};	// rows of the MVP
	float				prevWorldToClip[4][4] = {};	// of the frame before; this frame's when there is none
	float				origin[3] = {};
	float				pixelSpread = 0.0f;		// radians per pixel of the viewport's height: the texture level of a hit
	uint32				epoch = 0;				// changes when the history starts again: a new view, size, a gap, time going back
	uint32				sequence = 0;			// frames of the epoch before this one
	int					eye = 0;				// 0, or 1 for the second eye of a stereo pair
	bool				store = true;			// false: a screenshot reads the history and leaves it as it is
	bool				held = false;			// nothing of the frame could be traced: what the history holds of it stands
};

// r_rtReflection*: a mirror ray per pixel whose G-buffer may reflect, added to the HDR scene after the lights.
struct rtReflectionSettings_t {
	bool				enabled = false;	// with scene and gbuffer
	bool				alone = false;		// rtDebug_t::REFLECTION: the reflections replace the lights' sum
	bool				shadows = true;		// a shadow ray per light that lights a hit
	int					maxLights = 8;		// lights per hit, the first that reach it
	float				scale = 1.0f;
	float				distance = 2048.0f;	// the longest ray
	float				maxRoughness = 0.6f;	// rougher pixels send no ray; the weight fades in over 0.15 below it
};

// r_rtShadows: the main view's lights that cast shadows trace them in their interactions (R16, R17), a ray per
// lit pixel and light toward the light's disk, instead of drawing stencil shadow volumes.
// What is left of the lights is filtered before the scene takes it (R18): over the eye's history, then over the
// pixel's neighbours of the same surface, wider where the history still varies.
// The front end makes no stencil shadow volumes for such a view once the back end has traced its world's main view
// (volumes false): a frame that then cannot trace keeps the shadows its history holds, and the next has volumes.
struct rtShadowSettings_t {
	bool				enabled = false;	// with scene; never in a view that is not its frame's main one
	bool				volumes = true;		// the view's traced lights have their stencil shadow volumes
	bool				keepVolumes = false;	// r_rtShadowVolumes: they are made whatever the back end traced
	bool				show = false;		// rtDebug_t::SHADOWS
	bool				showLength = false;	// rtDebug_t::HISTORY
	bool				history = true;		// r_rtHistory; false: every frame alone, with the same noise
	bool				reference = false;	// r_rtReference: a plain average of up to maxHistory frames, 32-bit, unfiltered
	int					maxHistory = 16;	// r_rtShadowHistory: frames a pixel averages
	int					passes = 3;			// r_rtShadowPasses: spatial passes, each twice as wide; 0 with no history = raw
	// the filter reads the G-buffer
	[[nodiscard]] bool	Filtered() const { return enabled && ( history || passes > 0 ); }
	// A view the back end never traces (a subview, a portal sky's): its shadows are the stencil ones.
	void				KeepStencil() { enabled = false; volumes = true; }
	// no volume is made for a light the view would trace
	[[nodiscard]] bool	WithoutVolumes() const { return enabled && !volumes; }
};

// What tells the front end that it may leave the volumes out: the GPU scene, and the update of it, of which the
// back end's last frame traced the shadows of every view that counted on it. A frame that draws none, one it could
// not trace, or two scenes name none, and the views after it make their volumes again.
struct rtTracedScene_t {
	uint32				scene = 0;			// idGpuScene's id; 0 none
	uint32				sequence = 0;		// the earliest of its updates the frame traced
	constexpr bool		operator==( const rtTracedScene_t & ) const = default;
};

struct rtShadowLatch_t {
	rtTracedScene_t		traced;				// of the frame's views so far
	bool				failed = false;

	void				BeginFrame() { traced = {}; failed = false; }
	void				View( uint32 sceneId, uint32 sequence, bool wasTraced ) {
		failed = failed || !wasTraced || sceneId == 0 || ( traced.scene != 0 && traced.scene != sceneId );
		traced.sequence = ( traced.scene == 0 || sequence < traced.sequence ) ? sequence : traced.sequence;
		traced.scene = sceneId;
	}
	[[nodiscard]] rtTracedScene_t	EndFrame() const { return failed ? rtTracedScene_t() : traced; }
};

// The front end's side of R13, the view the back end takes for its frame's main one: the last view of a world that
// is not marked (viewDef_t::notMainView), before the first GUI view that draws, per eye in stereo. A world's view
// after that GUI view, or after a view of the same world that may be the main one (a second Draw of the frame: the
// wipe at a level's end), is marked: the first stays the main one, the second keeps its stencil shadows.
struct rtMainViewForecast_t {
	static constexpr int	MAX_WORLDS = 4;
	struct scan_t {
		bool			guiDrawn = false;
		bool			otherEye = false;			// the last view that names an eye names the other one
		int				numWorlds = 0;
		const void *	worlds[MAX_WORLDS] = {};	// those with a view that may be the main one

		[[nodiscard]] bool	Has( const void * world ) const {
			for ( int i = 0; i < numWorlds; i++ ) {
				if ( worlds[i] == world ) {
					return true;
				}
			}
			return false;
		}
	};
	scan_t				scans[3];		// the left eye's, a mono frame's, the right eye's

	void				BeginFrame() { *this = {}; }
	// Every draw view command of the frame, in order. world: of a view that may be the main one (it draws a world,
	// is no subview and is not marked), else null.
	void				AddView( int eye, bool gui, bool draws, const void * world ) {
		for ( int i = 0; i < 3; i++ ) {
			scan_t & scan = scans[i];
			if ( i != 1 && eye != 0 ) {
				scan.otherEye = ( eye != i - 1 );
				if ( scan.otherEye ) {
					continue;
				}
			}
			if ( !draws || scan.guiDrawn ) {
				continue;
			}
			if ( gui ) {
				scan.guiDrawn = !scan.otherEye;
			} else if ( world != nullptr && scan.numWorlds < MAX_WORLDS && !scan.Has( world ) ) {
				scan.worlds[scan.numWorlds++] = world;
			}
		}
	}
	// whether a view of world, about to be made, may be its frame's main one
	[[nodiscard]] bool	MayBeMain( int eye, const void * world ) const {
		const scan_t & scan = scans[( eye < 0 ) ? 0 : ( eye > 0 ) ? 2 : 1];
		return !scan.guiDrawn && !scan.Has( world );
	}
};

struct rayTracingSettings_t {
	bool				gbuffer = false;	// the main view writes the G-buffer: a consumer is on, HDR
	bool				scene = false;		// the main view gathers the GPU scene: a consumer is on, caps.rayTracing, HDR
	gbufferDebug_t		gbufferDebug = gbufferDebug_t::NONE;
	rtDebug_t			rtDebug = rtDebug_t::NONE;	// with scene
	rtReflectionSettings_t	reflection;
	rtShadowSettings_t	shadow;
	float				gloss = 1.0f;		// r_gbufferGloss: sqrt( specular luminance ) times this is the gloss
	float				lightSize = 1.0f;	// r_rtLightSize: scales every light's emitter; 0 = point lights
	int					frameCount = 0;		// the front end's frame: the motion is against the frame before
};
