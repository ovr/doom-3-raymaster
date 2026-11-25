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

struct rayTracingSettings_t {
	bool				gbuffer = false;	// the main view writes the G-buffer: a consumer is on, caps.gbuffer, HDR
	gbufferDebug_t		gbufferDebug = gbufferDebug_t::NONE;
	float				gloss = 1.0f;		// r_gbufferGloss: sqrt( specular luminance ) times this is the gloss
	int					frameCount = 0;		// the front end's frame: the motion is against the frame before
};
