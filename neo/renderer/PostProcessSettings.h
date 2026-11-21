// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

#include <array>

/*
	The post-process settings of a view. The front end fills them from the
	r_hdr* cvars into viewDef_t (R_DefaultPostProcessSettings); the back end hands those of the main view to
	idRenderBackend::BeginHdrScene and ResolveHdrScene, so a backend reads no cvar. Volumes in a map may
	override them per view later.

	  exposure        auto: the eye adaptation (d3d12_Hdr.h) from a histogram of EV100, its mean between
	                  lowPercent and highPercent clamped to minEV100 .. maxEV100, exposure 2^( compensation + bias
	                  - EV100 ) / 1.2; manual: 2^exposureCompensation
	  white balance   temperature and tint, a Bradford adaptation to the white of 6500 K; 6500 K and 0 are neutral
	  grading         saturation .. offset per range: shadows below shadowsMax, highlights above highlightsMin
	  tonemapper      CLAMP (debug: at 0 EV close to the LDR frame), AGX (agxLook 0 base, 1 punchy),
	                  FILMIC (a film curve in ACEScg: slope 0.88, toe 0.55, shoulder 0.26, white clip 0.04);
	                  each scaled so that the demos keep the LDR frames' mean brightness
	  bloom           bloomQuality + 1 levels from half the viewport down, each a Gaussian reaching its size times
	                  bloomSizeScale percent of the viewport's width, summed by the tints scaled to a luma of 1:
	                  the frame is lerped toward that by bloomIntensity, so a flat field stays as it is
	  local exposure  a base luminance per pixel, lerped by localBlurredBlend from a bilateral grid (64 x 64
	                  pixel cells, 32 bins of log luminance) toward a Gaussian of localBlurredKernel percent of the
	                  viewport's width; the base is scaled around mid grey by localHighlightContrast above it and
	                  localShadowContrast below, the pixel's detail around the base by localDetailStrength; all 1
	                  is the identity and costs nothing
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
	bool							autoExposure = true;
	float							autoExposureBias = -2.5f;	// EV of the auto exposure only: where the frame's mean lands
	float							minEV100 = -3.25f;			// the range of the target: dark scenes brighten by 0.5 EV at most
	float							maxEV100 = -1.25f;
	float							lowPercent = 0.1f;			// the part of the histogram's weight averaged, 0..1
	float							highPercent = 0.9f;
	float							speedUp = 3.0f;				// toward a brighter frame, per second: 1 - 2^( -seconds * speed )
	float							speedDown = 1.0f;
	float							meteringMask = 0.5f;		// 0 every pixel alike .. 1 the corners weigh exp( -2 )
	float							histogramMinEV = -8.0f;		// EV100 of the first bin's start and the last bin's end
	float							histogramMaxEV = 8.0f;
	uint8							visualize = 0;				// 1 the histogram and EVs, 2 false colour, 3 the bloom, 4 the local exposure
	float							whiteTemp = 6500.0f;
	float							whiteTint = 0.0f;
	colorGrade_t					global;
	colorGrade_t					shadows;
	colorGrade_t					midtones;
	colorGrade_t					highlights;
	float							shadowsMax = 0.09f;
	float							highlightsMin = 0.5f;
	hdrTonemapper_t					tonemapper = hdrTonemapper_t::AGX;
	uint8							agxLook = 1;
	bool							dither = true;		// ±0.5 of the output's last bit before it is quantized
	uint8							bloomQuality = 5;	// 0 none, 1..5: 2..6 levels
	float							bloomIntensity = 0.1f;
	float							bloomSizeScale = 4.0f;
	std::array<float, 6>			bloomSizes = { 0.3f, 1.0f, 2.0f, 10.0f, 30.0f, 64.0f };
	std::array<idVec3, 6>			bloomTints = { idVec3( 0.3465f, 0.3465f, 0.3465f ), idVec3( 0.138f, 0.138f, 0.138f ), idVec3( 0.1176f, 0.1176f, 0.1176f ),
		idVec3( 0.066f, 0.066f, 0.066f ), idVec3( 0.066f, 0.066f, 0.066f ), idVec3( 0.061f, 0.061f, 0.061f ) };
	bool							localExposure = true;
	float							localHighlightContrast = 0.8f;	// 1 neutral, below 1 compresses the highlights' range
	float							localShadowContrast = 1.0f;
	float							localDetailStrength = 1.0f;
	float							localBlurredBlend = 0.6f;		// 0 the grid alone, 1 the Gaussian alone
	float							localBlurredKernel = 50.0f;		// percent of the viewport's width, 6 sigma
};

// How the eye adaptation moves at a resolve: by seconds of the main view's time, or at once to the target.
struct hdrExposureStep_t {
	float							seconds = 0.0f;
	bool							reset = true;
};

// What the eye adaptation of a resolve the CPU has read back measured and chose.
struct hdrExposure_t {
	float							exposure = 1.0f;		// radiance times exposure is what the tonemapper sees
	float							measuredEV100 = 0.0f;
	float							adaptedEV100 = 0.0f;
	float							targetEV100 = 0.0f;
	float							preExposure = 1.0f;		// what that frame's scene was drawn with
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
