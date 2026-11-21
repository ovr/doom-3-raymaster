// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

/*
	The post-process settings of a view. The front end fills them from the
	r_hdr* cvars into viewDef_t (R_DefaultPostProcessSettings); the back end hands those of the main view to
	idRenderBackend::BeginHdrScene and ResolveHdrScene, so a backend reads no cvar. Volumes in a map may
	override them per view later.

	  exposure        manual: the scene times 2^exposureCompensation
	  white balance   temperature and tint, a Bradford adaptation to the white of 6500 K; 6500 K and 0 are neutral
	  grading         saturation .. offset per range: shadows below shadowsMax, highlights above highlightsMin
	  tonemapper      CLAMP (debug: at 0 EV close to the LDR frame), AGX (agxLook 0 base, 1 punchy),
	                  FILMIC (a film curve in ACEScg: slope 0.88, toe 0.55, shoulder 0.26, white clip 0.04)
*/

enum class hdrTonemapper_t : uint8 { CLAMP, AGX, FILMIC, COUNT };

// x, y, z per channel and w over all three.
struct colorGrade_t {
	idVec4							saturation = idVec4( 1.0f, 1.0f, 1.0f, 1.0f );
	idVec4							contrast = idVec4( 1.0f, 1.0f, 1.0f, 1.0f );
	idVec4							gamma = idVec4( 1.0f, 1.0f, 1.0f, 1.0f );
	idVec4							gain = idVec4( 1.0f, 1.0f, 1.0f, 1.0f );
	idVec4							offset = idVec4( 0.0f, 0.0f, 0.0f, 0.0f );
};

struct postProcessSettings_t {
	bool							hdr = false;		// r_hdr: the main view draws into the HDR scene, where the backend has one
	float							exposureCompensation = 0.0f;	// EV
	float							whiteTemp = 6500.0f;
	float							whiteTint = 0.0f;
	colorGrade_t					global;
	colorGrade_t					shadows;
	colorGrade_t					midtones;
	colorGrade_t					highlights;
	float							shadowsMax = 0.09f;
	float							highlightsMin = 0.5f;
	hdrTonemapper_t					tonemapper = hdrTonemapper_t::AGX;
	uint8							agxLook = 0;
	bool							dither = true;		// ±0.5 of the output's last bit before it is quantized
};

// The linear scene value a pre-exposed float scene holds per unit of radiance: SceneColor = radiance * PreExposure.
[[nodiscard]] inline float R_ManualExposure( const postProcessSettings_t & settings ) {
	return idMath::Pow( 2.0f, settings.exposureCompensation );
}

// linear → sRGB, the exact curve, also above 1 (include/srgb.hlsli).
[[nodiscard]] inline float R_LinearToSRGB( float c ) {
	return ( c <= 0.0031308f ) ? Max( c, 0.0f ) * 12.92f : 1.055f * idMath::Pow( c, 1.0f / 2.4f ) - 0.055f;
}

// sRGB → linear, the exact curve; above 1 the same formula goes on, so an overbright colour keeps its ratio.
[[nodiscard]] inline float R_SRGBToLinear( float c ) {
	return ( c <= 0.04045f ) ? c * ( 1.0f / 12.92f ) : idMath::Pow( ( c + 0.055f ) * ( 1.0f / 1.055f ), 2.4f );
}
