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
	COUNT
};

// The main view as its rays need it: GL's clip space of shared code, the eye, the angle of a pixel.
struct rtView_t {
	float				clipToWorld[4][4] = {};	// rows of the inverse of the view's MVP
	float				origin[3] = {};
	float				pixelSpread = 0.0f;		// radians per pixel of the viewport's height: the texture level of a hit
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

struct rayTracingSettings_t {
	bool				gbuffer = false;	// the main view writes the G-buffer: a consumer is on, caps.gbuffer, HDR
	bool				scene = false;		// the main view gathers the GPU scene: a consumer is on, caps.rayTracing, HDR
	gbufferDebug_t		gbufferDebug = gbufferDebug_t::NONE;
	rtDebug_t			rtDebug = rtDebug_t::NONE;	// with scene
	rtReflectionSettings_t	reflection;
	float				gloss = 1.0f;		// r_gbufferGloss: sqrt( specular luminance ) times this is the gloss
	int					frameCount = 0;		// the front end's frame: the motion is against the frame before
};
