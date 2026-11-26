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

#pragma hdrstop
#include "../idlib/precompiled.h"

#include <memory>

#include "tr_local.h"

// DeviceContext bypasses RenderSystem to work directly with this
idGuiModel * tr_guiModel;

// functions that are not called every frame
screenConfig_t	screenConfig;

idCVar r_debugContext( "r_debugContext", "0", CVAR_RENDERER, "read at startup: 1 = the Direct3D 12 debug layer, 2 = plus GPU-based validation, 3 = plus DRED" );
idCVar r_debugMaxDraws( "r_debugMaxDraws", "-1", CVAR_RENDERER | CVAR_INTEGER, "draw only the first N draws of every frame, -1 = all" );
idCVar r_hashDrawStream( "r_hashDrawStream", "0", CVAR_RENDERER | CVAR_BOOL, "print a hash of the draws of every frame, without handle values" );
idCVar r_testClearPattern( "r_testClearPattern", "0", CVAR_RENDERER | CVAR_BOOL, "end every frame with the clear pattern tools/vdemo/check-pattern.py checks" );
idCVar r_multiSamples( "r_multiSamples", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "number of antialiasing samples" );
idCVar r_vidMode( "r_vidMode", "0", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_INTEGER, "fullscreen video mode number" );
idCVar r_displayRefresh( "r_displayRefresh", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NOCHEAT, "optional display refresh rate option for vid mode", 0.0f, 240.0f );
idCVar r_fullscreen( "r_fullscreen", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "0 = windowed, 1 = full screen on monitor 1, 2 = full screen on monitor 2, etc" );
idCVar r_borderless( "r_borderless", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "with r_fullscreen > 0: 1 = borderless window at desktop resolution, 0 = exclusive fullscreen at r_vidMode" );
idCVar r_customWidth( "r_customWidth", "1280", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "custom screen width. set r_vidMode to -1 to activate" );
idCVar r_customHeight( "r_customHeight", "720", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "custom screen height. set r_vidMode to -1 to activate" );
idCVar r_windowX( "r_windowX", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "Non-fullscreen parameter" );
idCVar r_windowY( "r_windowY", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "Non-fullscreen parameter" );
idCVar r_windowWidth( "r_windowWidth", "1280", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "Non-fullscreen parameter" );
idCVar r_windowHeight( "r_windowHeight", "720", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "Non-fullscreen parameter" );

idCVar r_useViewBypass( "r_useViewBypass", "1", CVAR_RENDERER | CVAR_INTEGER, "bypass a frame of latency to the view" );
idCVar r_useLightPortalFlow( "r_useLightPortalFlow", "1", CVAR_RENDERER | CVAR_BOOL, "use a more precise area reference determination" );
idCVar r_singleTriangle( "r_singleTriangle", "0", CVAR_RENDERER | CVAR_BOOL, "only draw a single triangle per primitive" );
idCVar r_checkBounds( "r_checkBounds", "0", CVAR_RENDERER | CVAR_BOOL, "compare all surface bounds with precalculated ones" );
idCVar r_useConstantMaterials( "r_useConstantMaterials", "1", CVAR_RENDERER | CVAR_BOOL, "use pre-calculated material registers if possible" );
idCVar r_useSilRemap( "r_useSilRemap", "1", CVAR_RENDERER | CVAR_BOOL, "consider verts with the same XYZ, but different ST the same for shadows" );
idCVar r_useNodeCommonChildren( "r_useNodeCommonChildren", "1", CVAR_RENDERER | CVAR_BOOL, "stop pushing reference bounds early when possible" );
idCVar r_useShadowSurfaceScissor( "r_useShadowSurfaceScissor", "1", CVAR_RENDERER | CVAR_BOOL, "scissor shadows by the scissor rect of the interaction surfaces" );
idCVar r_useCachedDynamicModels( "r_useCachedDynamicModels", "1", CVAR_RENDERER | CVAR_BOOL, "cache snapshots of dynamic models" );
idCVar r_useSRGB( "r_useSRGB", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE, "1 = both texture and framebuffer, 2 = framebuffer only, 3 = texture only" );
idCVar r_maxAnisotropicFiltering( "r_maxAnisotropicFiltering", "8", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "limit aniso filtering" );
idCVar r_useTrilinearFiltering( "r_useTrilinearFiltering", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "Extra quality filtering" );
idCVar r_lodBias( "r_lodBias", "0.5", CVAR_RENDERER | CVAR_ARCHIVE, "image lod bias" );

idCVar r_useStateCaching( "r_useStateCaching", "1", CVAR_RENDERER | CVAR_BOOL, "0 = every State() changes every bit, so the depth test is no longer strict" );

idCVar r_znear( "r_znear", "3", CVAR_RENDERER | CVAR_FLOAT, "near Z clip plane distance", 0.001f, 200.0f );

idCVar r_swapInterval( "r_swapInterval", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "0 = tear, 1 and 2 = v-sync" );

idCVar r_gamma( "r_gamma", "1.0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "changes gamma tables", 0.5f, 3.0f );
idCVar r_brightness( "r_brightness", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "changes gamma tables", 0.5f, 2.0f );

idCVar r_jitter( "r_jitter", "0", CVAR_RENDERER | CVAR_BOOL, "randomly subpixel jitter the projection matrix" );

idCVar r_skipSuppress( "r_skipSuppress", "0", CVAR_RENDERER | CVAR_BOOL, "ignore the per-view suppressions" );
idCVar r_skipPostProcess( "r_skipPostProcess", "0", CVAR_RENDERER | CVAR_BOOL, "skip all post-process renderings" );
idCVar r_skipInteractions( "r_skipInteractions", "0", CVAR_RENDERER | CVAR_BOOL, "skip all light/surface interaction drawing" );
idCVar r_skipDynamicTextures( "r_skipDynamicTextures", "0", CVAR_RENDERER | CVAR_BOOL, "don't dynamically create textures" );
idCVar r_skipCopyTexture( "r_skipCopyTexture", "0", CVAR_RENDERER | CVAR_BOOL, "do all rendering, but don't actually copyTexSubImage2D" );
idCVar r_skipBackEnd( "r_skipBackEnd", "0", CVAR_RENDERER | CVAR_BOOL, "don't draw anything" );
idCVar r_skipRender( "r_skipRender", "0", CVAR_RENDERER | CVAR_BOOL, "skip 3D rendering, but pass 2D" );
idCVar r_skipTranslucent( "r_skipTranslucent", "0", CVAR_RENDERER | CVAR_BOOL, "skip the translucent interaction rendering" );
idCVar r_skipAmbient( "r_skipAmbient", "0", CVAR_RENDERER | CVAR_BOOL, "bypasses all non-interaction drawing" );
idCVar r_skipNewAmbient( "r_skipNewAmbient", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE, "bypasses all vertex/fragment program ambient drawing" );
idCVar r_skipBlendLights( "r_skipBlendLights", "0", CVAR_RENDERER | CVAR_BOOL, "skip all blend lights" );
idCVar r_skipFogLights( "r_skipFogLights", "0", CVAR_RENDERER | CVAR_BOOL, "skip all fog lights" );
idCVar r_skipDeforms( "r_skipDeforms", "0", CVAR_RENDERER | CVAR_BOOL, "leave all deform materials in their original state" );
idCVar r_skipFrontEnd( "r_skipFrontEnd", "0", CVAR_RENDERER | CVAR_BOOL, "bypasses all front end work, but 2D gui rendering still draws" );
idCVar r_skipUpdates( "r_skipUpdates", "0", CVAR_RENDERER | CVAR_BOOL, "1 = don't accept any entity or light updates, making everything static" );
idCVar r_interpolate( "r_interpolate", "1", CVAR_RENDERER | CVAR_BOOL, "draw the entities that moved in the last game tic between their two tic poses, 0 = the tic poses" );
idCVar r_interpolateSnapDist( "r_interpolateSnapDist", "64", CVAR_RENDERER | CVAR_FLOAT, "an entity that moves farther than this in one tic is drawn at its tic pose, 0 = no limit" );
idCVar r_skipDecals( "r_skipDecals", "0", CVAR_RENDERER | CVAR_BOOL, "skip decal surfaces" );
idCVar r_skipOverlays( "r_skipOverlays", "0", CVAR_RENDERER | CVAR_BOOL, "skip overlay surfaces" );
idCVar r_skipSpecular( "r_skipSpecular", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_CHEAT | CVAR_ARCHIVE, "use black for specular1" );
idCVar r_skipBump( "r_skipBump", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE, "uses a flat surface instead of the bump map" );
idCVar r_skipDiffuse( "r_skipDiffuse", "0", CVAR_RENDERER | CVAR_BOOL, "use black for diffuse" );
idCVar r_skipSubviews( "r_skipSubviews", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = don't render any gui elements on surfaces" );
idCVar r_skipGuiShaders( "r_skipGuiShaders", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = skip all gui elements on surfaces, 2 = skip drawing but still handle events, 3 = draw but skip events", 0, 3, idCmdSystem::ArgCompletion_Integer<0,3> );
idCVar r_skipParticles( "r_skipParticles", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = skip all particle systems", 0, 1, idCmdSystem::ArgCompletion_Integer<0,1> );
idCVar r_skipShadows( "r_skipShadows", "0", CVAR_RENDERER | CVAR_BOOL  | CVAR_ARCHIVE, "disable shadows" );

idCVar r_useLightPortalCulling( "r_useLightPortalCulling", "1", CVAR_RENDERER | CVAR_INTEGER, "0 = none, 1 = cull frustum corners to plane, 2 = exact clip the frustum faces", 0, 2, idCmdSystem::ArgCompletion_Integer<0,2> );
idCVar r_useLightAreaCulling( "r_useLightAreaCulling", "1", CVAR_RENDERER | CVAR_BOOL, "0 = off, 1 = on" );
idCVar r_useLightScissors( "r_useLightScissors", "3", CVAR_RENDERER | CVAR_INTEGER, "0 = no scissor, 1 = non-clipped scissor, 2 = near-clipped scissor, 3 = fully-clipped scissor", 0, 3, idCmdSystem::ArgCompletion_Integer<0,3> );
idCVar r_useEntityPortalCulling( "r_useEntityPortalCulling", "1", CVAR_RENDERER | CVAR_INTEGER, "0 = none, 1 = cull frustum corners to plane, 2 = exact clip the frustum faces", 0, 2, idCmdSystem::ArgCompletion_Integer<0,2> );
idCVar r_logFile( "r_logFile", "0", CVAR_RENDERER | CVAR_INTEGER, "number of frames to write the render log of" );
idCVar r_clear( "r_clear", "0", CVAR_RENDERER, "clear the screen at the start of every frame: 0 = no (a stereo eye is cleared black), 1 = purple, 2 = black, 'r g b' = custom" );

idCVar r_offsetFactor( "r_offsetfactor", "0", CVAR_RENDERER | CVAR_FLOAT, "polygon offset parameter" );
idCVar r_offsetUnits( "r_offsetunits", "-600", CVAR_RENDERER | CVAR_FLOAT, "polygon offset parameter" );

idCVar r_shadowPolygonOffset( "r_shadowPolygonOffset", "-1", CVAR_RENDERER | CVAR_FLOAT, "bias value added to depth test for stencil shadow drawing" );
idCVar r_shadowPolygonFactor( "r_shadowPolygonFactor", "0", CVAR_RENDERER | CVAR_FLOAT, "scale value for stencil shadow drawing" );
idCVar r_subviewOnly( "r_subviewOnly", "0", CVAR_RENDERER | CVAR_BOOL, "1 = don't render main view, allowing subviews to be debugged" );
idCVar r_lightScale( "r_lightScale", "3", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_FLOAT, "all light intensities are multiplied by this" );

// PostProcessSettings.h: the front end copies them into every view, the back end resolves the main view's
idCVar r_hdr( "r_hdr", "1", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_BOOL, "1 = the 3D view is lit in linear HDR and tonemapped (where the render API has it), 0 = the LDR frame" );
idCVar r_hdrTonemap( "r_hdrTonemap", "1", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_INTEGER, "HDR tonemapper: 0 = clamp, 1 = AgX, 2 = a film curve in ACEScg", 0, 2 );
idCVar r_hdrAgxLook( "r_hdrAgxLook", "0", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_INTEGER, "AgX look: 0 = base, 1 = punchy (deeper blacks, more saturated light)", 0, 1 );
idCVar r_hdrExposureCompensation( "r_hdrExposureCompensation", "0", CVAR_RENDERER | CVAR_FLOAT, "EV added to the HDR exposure", -8.0f, 8.0f );
idCVar r_hdrWhiteTemp( "r_hdrWhiteTemp", "6500", CVAR_RENDERER | CVAR_FLOAT, "HDR white balance temperature in kelvin, 6500 = neutral", 1500.0f, 15000.0f );
idCVar r_hdrWhiteTint( "r_hdrWhiteTint", "0", CVAR_RENDERER | CVAR_FLOAT, "HDR white balance tint, -1 = green .. 1 = magenta", -1.0f, 1.0f );
idCVar r_hdrSaturation( "r_hdrSaturation", "1", CVAR_RENDERER | CVAR_FLOAT, "HDR colour grading saturation", 0.0f, 2.0f );
idCVar r_hdrContrast( "r_hdrContrast", "1", CVAR_RENDERER | CVAR_FLOAT, "HDR colour grading contrast around mid grey", 0.0f, 2.0f );
idCVar r_hdrDither( "r_hdrDither", "1", CVAR_RENDERER | CVAR_BOOL, "dither the tonemapped HDR frame before it is quantized to 8 bits" );
idCVar r_hdrGammaBlend( "r_hdrGammaBlend", "1", CVAR_RENDERER | CVAR_BOOL, "1 = in HDR, the passes after the lights (unlit stages, translucent surfaces, fog and blend lights) blend in gamma as the content was made for; 0 = in linear" );
idCVar r_hdrDebug( "r_hdrDebug", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = print the eye adaptation once a second, 2 = print the HDR phase of every frame, 3 = print the eye adaptation of every frame", 0, 3 );
idCVar r_hdrAutoExposure( "r_hdrAutoExposure", "1", CVAR_RENDERER | CVAR_BOOL, "1 = the HDR exposure follows the frame's brightness (eye adaptation), 0 = only r_hdrExposureCompensation" );
idCVar r_hdrAutoExposureBias( "r_hdrAutoExposureBias", "-2.5", CVAR_RENDERER | CVAR_FLOAT, "EV added to the auto exposure only: where a frame's mean brightness lands", -8.0f, 8.0f );
idCVar r_hdrMinEV100( "r_hdrMinEV100", "-3.25", CVAR_RENDERER | CVAR_FLOAT, "the darkest frame the eye adapts to, EV100", -16.0f, 16.0f );
idCVar r_hdrMaxEV100( "r_hdrMaxEV100", "-1.25", CVAR_RENDERER | CVAR_FLOAT, "the brightest frame the eye adapts to, EV100", -16.0f, 16.0f );
idCVar r_hdrSpeedUp( "r_hdrSpeedUp", "3", CVAR_RENDERER | CVAR_FLOAT, "eye adaptation toward a brighter frame, per second", 0.01f, 100.0f );
idCVar r_hdrSpeedDown( "r_hdrSpeedDown", "1", CVAR_RENDERER | CVAR_FLOAT, "eye adaptation toward a darker frame, per second", 0.01f, 100.0f );
idCVar r_hdrLowPercent( "r_hdrLowPercent", "10", CVAR_RENDERER | CVAR_FLOAT, "the eye adaptation averages the histogram from this percent of its weight ..", 0.0f, 99.0f );
idCVar r_hdrHighPercent( "r_hdrHighPercent", "90", CVAR_RENDERER | CVAR_FLOAT, ".. to this one", 1.0f, 100.0f );
idCVar r_hdrMeteringMask( "r_hdrMeteringMask", "0.5", CVAR_RENDERER | CVAR_FLOAT, "0 = every pixel counts the same in the eye adaptation, 1 = the corners count exp( -2 ) of the centre", 0.0f, 1.0f );
idCVar r_hdrHistogramMinEV( "r_hdrHistogramMinEV", "-8", CVAR_RENDERER | CVAR_FLOAT, "EV100 of the start of the exposure histogram; darker pixels do not count", -16.0f, 16.0f );
idCVar r_hdrHistogramMaxEV( "r_hdrHistogramMaxEV", "8", CVAR_RENDERER | CVAR_FLOAT, "EV100 of the end of the exposure histogram", -16.0f, 16.0f );
idCVar r_hdrVisualize( "r_hdrVisualize", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = the exposure histogram and EVs over the HDR frame, 2 = false colour in stops around mid grey, 3 = the bloom alone, 4 = the local exposure", 0, 4 );
idCVar r_hdrBloomQuality( "r_hdrBloomQuality", "5", CVAR_RENDERER | CVAR_INTEGER, "the HDR bloom: 0 = none, 1..5 = 2..6 levels from half the viewport down", 0, 5 );
idCVar r_hdrBloomIntensity( "r_hdrBloomIntensity", "0.1", CVAR_RENDERER | CVAR_FLOAT, "how far the HDR frame is lerped toward its bloom, which keeps its energy", 0.0f, 1.0f );
idCVar r_hdrBloomSizeScale( "r_hdrBloomSizeScale", "4", CVAR_RENDERER | CVAR_FLOAT, "scales the reach of every bloom level, in percent of the viewport's width", 0.0f, 32.0f );
idCVar r_hdrLocalExposure( "r_hdrLocalExposure", "1", CVAR_RENDERER | CVAR_BOOL, "0 turns the HDR local exposure off whatever its contrasts" );
idCVar r_hdrLocalHighlightContrast( "r_hdrLocalHighlightContrast", "0.8", CVAR_RENDERER | CVAR_FLOAT, "local exposure: the contrast of the base above mid grey, 1 = as it is", 0.0f, 2.0f );
idCVar r_hdrLocalShadowContrast( "r_hdrLocalShadowContrast", "1", CVAR_RENDERER | CVAR_FLOAT, "local exposure: the contrast of the base below mid grey, 1 = as it is", 0.0f, 2.0f );
idCVar r_hdrLocalDetail( "r_hdrLocalDetail", "1", CVAR_RENDERER | CVAR_FLOAT, "local exposure: the strength of the detail around the base, 1 = as it is", 0.0f, 4.0f );
idCVar r_hdrLocalBlurredBlend( "r_hdrLocalBlurredBlend", "0.6", CVAR_RENDERER | CVAR_FLOAT, "local exposure: 0 = the bilateral grid's base, 1 = the blurred luminance's", 0.0f, 1.0f );
idCVar r_hdrLocalBlurredKernel( "r_hdrLocalBlurredKernel", "50", CVAR_RENDERER | CVAR_FLOAT, "local exposure: the blurred luminance's kernel, percent of the viewport's width", 1.0f, 200.0f );

postProcessSettings_t R_DefaultPostProcessSettings() {
	postProcessSettings_t settings;
	settings.hdr = r_hdr.GetBool();
	settings.exposureCompensation = r_hdrExposureCompensation.GetFloat();
	settings.autoExposure = r_hdrAutoExposure.GetBool();
	settings.autoExposureBias = r_hdrAutoExposureBias.GetFloat();
	settings.minEV100 = r_hdrMinEV100.GetFloat();
	settings.maxEV100 = r_hdrMaxEV100.GetFloat();
	settings.speedUp = r_hdrSpeedUp.GetFloat();
	settings.speedDown = r_hdrSpeedDown.GetFloat();
	settings.lowPercent = r_hdrLowPercent.GetFloat() * 0.01f;
	settings.highPercent = r_hdrHighPercent.GetFloat() * 0.01f;
	settings.meteringMask = r_hdrMeteringMask.GetFloat();
	settings.histogramMinEV = r_hdrHistogramMinEV.GetFloat();
	settings.histogramMaxEV = r_hdrHistogramMaxEV.GetFloat();
	settings.visualize = static_cast<uint8>( r_hdrVisualize.GetInteger() );
	settings.whiteTemp = r_hdrWhiteTemp.GetFloat();
	settings.whiteTint = r_hdrWhiteTint.GetFloat();
	settings.global.saturation.w = r_hdrSaturation.GetFloat();
	settings.global.contrast.w = r_hdrContrast.GetFloat();
	settings.tonemapper = static_cast<hdrTonemapper_t>( r_hdrTonemap.GetInteger() );
	settings.agxLook = static_cast<uint8>( r_hdrAgxLook.GetInteger() );
	settings.dither = r_hdrDither.GetBool();
	settings.bloomQuality = static_cast<uint8>( r_hdrBloomQuality.GetInteger() );
	settings.bloomIntensity = r_hdrBloomIntensity.GetFloat();
	settings.bloomSizeScale = r_hdrBloomSizeScale.GetFloat();
	settings.localExposure = r_hdrLocalExposure.GetBool();
	settings.localHighlightContrast = r_hdrLocalHighlightContrast.GetFloat();
	settings.localShadowContrast = r_hdrLocalShadowContrast.GetFloat();
	settings.localDetailStrength = r_hdrLocalDetail.GetFloat();
	settings.localBlurredBlend = r_hdrLocalBlurredBlend.GetFloat();
	settings.localBlurredKernel = r_hdrLocalBlurredKernel.GetFloat();
	return settings;
}

// RayTracingSettings.h: copied into every view like the post process settings
idCVar r_gbufferDebug( "r_gbufferDebug", "0", CVAR_RENDERER | CVAR_INTEGER, "shows the main view's thin G-buffer: 1 = the world normal, 2 = F0, 3 = the roughness, 4 = the motion (hue = direction, 8 pixels = full), 5 = the albedo", 0, 5 );
idCVar r_gbufferGloss( "r_gbufferGloss", "1.2", CVAR_RENDERER | CVAR_FLOAT, "the G-buffer's gloss is the square root of the specular luminance times this; the roughness is 1 - gloss", 0.0f, 4.0f );
idCVar r_rtDebug( "r_rtDebug", "0", CVAR_RENDERER | CVAR_INTEGER, "shows what a ray per pixel of the main view hits: 1 = the instances, 2 = the rays against the depth (grey agrees, red misses a drawn surface, blue hits in front of one, yellow a back face), 3 = the hit normal, 4 = the hit's diffuse colour, 5 = the mirror reflections alone", 0, 5 );
idCVar r_rtLightSize( "r_rtLightSize", "1", CVAR_RENDERER | CVAR_FLOAT, "scales the emitter of every light for the ray traced shadows (its light_size, by default 2 % of the light's reach within 2 .. 10 units); 0 = point lights", 0.0f, 16.0f );
idCVar r_rtReflections( "r_rtReflections", "0", CVAR_RENDERER | CVAR_BOOL, "ray traced mirror reflections of the main view on the surfaces whose specular may reflect" );
idCVar r_rtReflectionScale( "r_rtReflectionScale", "1", CVAR_RENDERER | CVAR_FLOAT, "scales the ray traced reflections", 0.0f, 4.0f );
idCVar r_rtReflectionDistance( "r_rtReflectionDistance", "2048", CVAR_RENDERER | CVAR_FLOAT, "the longest reflection ray, in world units", 16.0f, 65536.0f );
idCVar r_rtReflectionLights( "r_rtReflectionLights", "8", CVAR_RENDERER | CVAR_INTEGER, "lights that may light a reflected point, the first that reach it", 0, 64 );
idCVar r_rtReflectionShadows( "r_rtReflectionShadows", "1", CVAR_RENDERER | CVAR_BOOL, "a shadow ray toward every light of a reflected point" );
idCVar r_rtReflectionRoughness( "r_rtReflectionRoughness", "0.6", CVAR_RENDERER | CVAR_FLOAT, "pixels of this G-buffer roughness and rougher reflect nothing; the reflection fades in over 0.15 below it", 0.0f, 1.0f );

rayTracingSettings_t R_DefaultRayTracingSettings( const postProcessSettings_t & postProcess ) {
	rayTracingSettings_t settings;
	settings.gbufferDebug = static_cast<gbufferDebug_t>( r_gbufferDebug.GetInteger() );
	settings.gloss = r_gbufferGloss.GetFloat();
	settings.lightSize = r_rtLightSize.GetFloat();
	settings.frameCount = tr.frameCount;
	const bool available = ( renderBackend != nullptr ) && postProcess.hdr;
	const bool tracing = ( renderBackend != nullptr ) && renderBackend->Caps().rayTracing && postProcess.hdr;
	settings.rtDebug = tracing ? static_cast<rtDebug_t>( r_rtDebug.GetInteger() ) : rtDebug_t::NONE;

	rtReflectionSettings_t & reflection = settings.reflection;
	reflection.alone = ( settings.rtDebug == rtDebug_t::REFLECTION );
	reflection.enabled = tracing && available && ( r_rtReflections.GetBool() || reflection.alone );
	reflection.shadows = r_rtReflectionShadows.GetBool();
	reflection.maxLights = r_rtReflectionLights.GetInteger();
	reflection.scale = r_rtReflectionScale.GetFloat();
	reflection.distance = r_rtReflectionDistance.GetFloat();
	reflection.maxRoughness = r_rtReflectionRoughness.GetFloat();

	settings.gbuffer = available && ( settings.gbufferDebug != gbufferDebug_t::NONE || reflection.enabled );
	settings.scene = tracing && ( r_rtStats.GetBool() || settings.rtDebug != rtDebug_t::NONE || reflection.enabled );
	return settings;
}

idCVar r_flareSize( "r_flareSize", "1", CVAR_RENDERER | CVAR_FLOAT, "scale the flare deforms from the material def" ); 

idCVar r_skipPrelightShadows( "r_skipPrelightShadows", "0", CVAR_RENDERER | CVAR_BOOL, "skip the dmap generated static shadow volumes" );
idCVar r_useScissor( "r_useScissor", "1", CVAR_RENDERER | CVAR_BOOL, "scissor clip as portals and lights are processed" );
idCVar r_useLightDepthBounds( "r_useLightDepthBounds", "1", CVAR_RENDERER | CVAR_BOOL, "use depth bounds test on lights to reduce both shadow and interaction fill" );
idCVar r_useShadowDepthBounds( "r_useShadowDepthBounds", "1", CVAR_RENDERER | CVAR_BOOL, "use depth bounds test on individual shadow volumes to reduce shadow fill" );

idCVar r_screenFraction( "r_screenFraction", "100", CVAR_RENDERER | CVAR_INTEGER, "for testing fill rate, the resolution of the entire screen can be changed" );
idCVar r_usePortals( "r_usePortals", "1", CVAR_RENDERER | CVAR_BOOL, " 1 = use portals to perform area culling, otherwise draw everything" );
idCVar r_singleLight( "r_singleLight", "-1", CVAR_RENDERER | CVAR_INTEGER, "suppress all but one light" );
idCVar r_singleEntity( "r_singleEntity", "-1", CVAR_RENDERER | CVAR_INTEGER, "suppress all but one entity" );
idCVar r_singleSurface( "r_singleSurface", "-1", CVAR_RENDERER | CVAR_INTEGER, "suppress all but one surface on each entity" );
idCVar r_singleArea( "r_singleArea", "0", CVAR_RENDERER | CVAR_BOOL, "only draw the portal area the view is actually in" );
idCVar r_lightAllBackFaces( "r_lightAllBackFaces", "0", CVAR_RENDERER | CVAR_BOOL, "light all the back faces, even when they would be shadowed" );

// visual debugging info
idCVar r_showPortals( "r_showPortals", "0", CVAR_RENDERER | CVAR_BOOL, "draw portal outlines in color based on passed / not passed" );
idCVar r_showUpdates( "r_showUpdates", "0", CVAR_RENDERER | CVAR_BOOL, "report entity and light updates and ref counts" );
idCVar r_showDemo( "r_showDemo", "0", CVAR_RENDERER | CVAR_BOOL, "report reads and writes to the demo file" );
idCVar r_showDynamic( "r_showDynamic", "0", CVAR_RENDERER | CVAR_BOOL, "report stats on dynamic surface generation" );
idCVar r_showInterpolation( "r_showInterpolation", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = boxes of the interpolated entities: previous tic red, current tic green, drawn yellow, 2 = also view-attached entities and lights", 0, 2, idCmdSystem::ArgCompletion_Integer<0,2> );
idCVar r_showLights( "r_showLights", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = just print volumes numbers, highlighting ones covering the view, 2 = also draw planes of each volume, 3 = also draw edges of each volume", 0, 3, idCmdSystem::ArgCompletion_Integer<0,3> );
idCVar r_showShadows( "r_showShadows", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = visualize the stencil shadow volumes, 2 = draw filled in", 0, 3, idCmdSystem::ArgCompletion_Integer<0,3> );
idCVar r_showLightScissors( "r_showLightScissors", "0", CVAR_RENDERER | CVAR_BOOL, "show light scissor rectangles" );
idCVar r_showLightCount( "r_showLightCount", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = colors surfaces based on light count, 2 = also count everything through walls", 0, 2, idCmdSystem::ArgCompletion_Integer<0,2> );
idCVar r_showViewEntitys( "r_showViewEntitys", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = displays the bounding boxes of all view models, 2 = print index numbers" );
idCVar r_showTris( "r_showTris", "0", CVAR_RENDERER | CVAR_INTEGER, "enables wireframe rendering of the world, 1 = only draw visible ones, 2 = draw all front facing, 3 = draw all, 4 = draw with alpha", 0, 4, idCmdSystem::ArgCompletion_Integer<0,4> );
idCVar r_showSurfaceInfo( "r_showSurfaceInfo", "0", CVAR_RENDERER | CVAR_BOOL, "show surface material name under crosshair" );
idCVar r_showNormals( "r_showNormals", "0", CVAR_RENDERER | CVAR_FLOAT, "draws wireframe normals" );
idCVar r_showMemory( "r_showMemory", "0", CVAR_RENDERER | CVAR_BOOL, "print frame memory utilization" );
idCVar r_showCull( "r_showCull", "0", CVAR_RENDERER | CVAR_BOOL, "report sphere and box culling stats" );
idCVar r_showAddModel( "r_showAddModel", "0", CVAR_RENDERER | CVAR_BOOL, "report stats from tr_addModel" );
idCVar r_showSurfaces( "r_showSurfaces", "0", CVAR_RENDERER | CVAR_BOOL, "report surface/light/shadow counts" );
idCVar r_showPrimitives( "r_showPrimitives", "0", CVAR_RENDERER | CVAR_INTEGER, "report drawsurf/index/vertex counts" );

idCVar r_useEntityCallbacks( "r_useEntityCallbacks", "1", CVAR_RENDERER | CVAR_BOOL, "if 0, issue the callback immediately at update time, rather than defering" );

idCVar r_showSkel( "r_showSkel", "0", CVAR_RENDERER | CVAR_INTEGER, "draw the skeleton when model animates, 1 = draw model with skeleton, 2 = draw skeleton only", 0, 2, idCmdSystem::ArgCompletion_Integer<0,2> );
idCVar r_jointNameScale( "r_jointNameScale", "0.02", CVAR_RENDERER | CVAR_FLOAT, "size of joint names when r_showskel is set to 1" );
idCVar r_jointNameOffset( "r_jointNameOffset", "0.5", CVAR_RENDERER | CVAR_FLOAT, "offset of joint names when r_showskel is set to 1" );

idCVar r_debugLineDepthTest( "r_debugLineDepthTest", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "perform depth test on debug lines" );
idCVar r_debugArrowStep( "r_debugArrowStep", "120", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "step size of arrow cone line rotation in degrees", 0, 120 );
idCVar r_debugPolygonFilled( "r_debugPolygonFilled", "1", CVAR_RENDERER | CVAR_BOOL, "draw a filled polygon" );

idCVar r_materialOverride( "r_materialOverride", "", CVAR_RENDERER, "overrides all materials", idCmdSystem::ArgCompletion_Decl<DECL_MATERIAL> );

idCVar r_debugRenderToTexture( "r_debugRenderToTexture", "0", CVAR_RENDERER | CVAR_INTEGER, "" );

idCVar stereoRender_enable( "stereoRender_enable", "0", CVAR_INTEGER | CVAR_ARCHIVE, "1 = side-by-side compressed, 2 = top and bottom compressed, 3 = side-by-side, 4 = interlaced, 6 = 720 frame packed" );
idCVar stereoRender_swapEyes( "stereoRender_swapEyes", "0", CVAR_BOOL | CVAR_ARCHIVE, "reverse eye adjustments" );
idCVar stereoRender_deGhost( "stereoRender_deGhost", "0.05", CVAR_FLOAT | CVAR_ARCHIVE, "subtract from opposite eye to reduce ghosting" );


static bool r_initialized = false;

/*
=============================
R_IsInitialized
=============================
*/
bool R_IsInitialized() {
	return r_initialized;
}

/*
=============================
R_SetNewMode

r_fullScreen -1		borderless window at exact desktop coordinates
r_fullScreen 0		bordered window at exact desktop coordinates
r_fullScreen 1		fullscreen on monitor 1 at r_vidMode
r_fullScreen 2		fullscreen on monitor 2 at r_vidMode
...

r_borderless 1		with r_fullScreen > 0: borderless window at the monitor's desktop resolution
r_borderless 0		with r_fullScreen > 0: exclusive fullscreen at r_vidMode

r_vidMode -1		use r_customWidth / r_customHeight, even if they don't appear on the mode list
r_vidMode 0			use first mode returned by EnumDisplaySettings()
r_vidMode 1			use second mode returned by EnumDisplaySettings()
...

r_displayRefresh 0	don't specify refresh
r_displayRefresh 70	specify 70 hz, etc
=============================
*/
void R_SetNewMode( const bool fullInit ) {
	// the settings, then safe mode
	idStr failure = "no window with these settings";
	for ( int i = 0 ; i < 2 ; i++ ) {
		windowParms_t	parms;

		if ( r_fullscreen.GetInteger() <= 0 ) {
			// use explicit position / size for window
			parms.x = r_windowX.GetInteger();
			parms.y = r_windowY.GetInteger();
			parms.width = r_windowWidth.GetInteger();
			parms.height = r_windowHeight.GetInteger();
			// may still be -1 to force a borderless window
			parms.fullScreen = r_fullscreen.GetInteger();
			parms.borderless = false;
			parms.displayHz = 0;		// ignored
		} else {
			// get the mode list for this monitor
			idList<vidMode_t> modeList;
			if ( !R_GetModeListForDisplay( r_fullscreen.GetInteger()-1, modeList ) ) {
				idLib::Printf( "r_fullscreen reset from %i to 1 because mode list failed.", r_fullscreen.GetInteger() );
				r_fullscreen.SetInteger( 1 );
				R_GetModeListForDisplay( r_fullscreen.GetInteger()-1, modeList );
			}
			if ( modeList.Num() < 1 ) {
				idLib::Printf( "Going to safe mode because mode list failed." );
				goto safeMode;
			}

			parms.x = 0;		// ignored
			parms.y = 0;		// ignored
			parms.fullScreen = r_fullscreen.GetInteger();
			// borderless uses the desktop resolution of the monitor, the
			// mode below is only a fallback
			parms.borderless = r_borderless.GetBool();

			// set the parameters we are trying
			if ( r_vidMode.GetInteger() < 0 ) {
				// try forcing a specific mode, even if it isn't on the list
				parms.width = r_customWidth.GetInteger();
				parms.height = r_customHeight.GetInteger();
				parms.displayHz = r_displayRefresh.GetInteger();
			} else {
				if ( r_vidMode.GetInteger() > modeList.Num() ) {
					idLib::Printf( "r_vidMode reset from %i to 0.\n", r_vidMode.GetInteger() );
					r_vidMode.SetInteger( 0 );
				}

				parms.width = modeList[ r_vidMode.GetInteger() ].width;
				parms.height = modeList[ r_vidMode.GetInteger() ].height;
				parms.displayHz = modeList[ r_vidMode.GetInteger() ].displayHz;
			}
		}

		if ( fullInit ) {
			// create the window, then the device on it
			if ( Sys_CreateWindow( parms ) ) {
				renderBackendInit_t init = {};
				init.nativeWindow = Sys_GetNativeWindow();
				init.width = screenConfig.nativeScreenWidth;
				init.height = screenConfig.nativeScreenHeight;
				init.multiSamples = r_multiSamples.GetInteger();
				const std::expected<void, idStr> started = renderBackend->Init( init );
				if ( started ) {
					break;
				}
				failure = started.error();
				common->Warning( "Direct3D 12: %s", failure.c_str() );
				Sys_DestroyWindow();
			}
		} else {
			// just rebuild the window
			if ( Sys_SetWindowMode( parms ) ) {
				// it worked
				renderBackend->ResizeOutput( screenConfig.nativeScreenWidth, screenConfig.nativeScreenHeight );
				break;
			}
		}

		if ( i == 1 ) {
			common->FatalError( "Unable to initialize Direct3D 12: %s", failure.c_str() );
		}

safeMode:
		// if we failed, set everything back to "safe mode"
		// and try again
		r_vidMode.SetInteger( 0 );
		r_fullscreen.SetInteger( 1 );
		r_displayRefresh.SetInteger( 0 );
		r_multiSamples.SetInteger( 0 );
	}
}

/*
==================
R_InitBackend

Creates the window (Sys_CreateWindow) and the device of the render backend on it
(renderBackend->Init), then the programs, the vertex cache and the frame data.

Changes to the vertex cache size or smp state require a vid_restart.

If R_IsInitialized() is false, no rendering can take place, but
all renderSystem functions will still operate properly, notably the material
and model information functions.
==================
*/
void R_InitBackend() {

	common->Printf( "----- R_InitBackend -----\n" );

	if ( R_IsInitialized() ) {
		common->FatalError( "R_InitBackend called while active" );
	}

	if ( renderBackend == nullptr ) {
		renderBackend = R_CreateRenderBackend();
	}

	// the window, and the device of the render backend on it
	R_SetNewMode( true );

	// input and sound systems need to be tied to the new window
	Sys_InitInput();

	r_initialized = true;

	renderProgManager.Init();

	// allocate the vertex array range or vertex objects
	vertexCache.Init();

	// allocate the frame data, which may be more if smp is enabled
	R_InitFrameData();

	// Reset our gamma
	R_SetColorMappings();
}

/*
=====================
R_ReloadSurface_f

Reload the material displayed by r_showSurfaceInfo
=====================
*/
static void R_ReloadSurface_f( const idCmdArgs &args ) {
	modelTrace_t mt;
	idVec3 start, end;
	
	// start far enough away that we don't hit the player model
	start = tr.primaryView->renderView.vieworg + tr.primaryView->renderView.viewaxis[0] * 16;
	end = start + tr.primaryView->renderView.viewaxis[0] * 1000.0f;
	if ( !tr.primaryWorld->Trace( mt, start, end, 0.0f, false ) ) {
		return;
	}

	common->Printf( "Reloading %s\n", mt.material->GetName() );

	// reload the decl
	mt.material->base->Reload();

	// reload any images used by the decl
	mt.material->ReloadImages( false );
}

/*
==============
R_ListModes_f
==============
*/
static void R_ListModes_f( const idCmdArgs &args ) {
	for ( int displayNum = 0 ; ; displayNum++ ) {
		idList<vidMode_t> modeList;
		if ( !R_GetModeListForDisplay( displayNum, modeList ) ) {
			break;
		}
		for ( int i = 0; i < modeList.Num() ; i++ ) {
			common->Printf( "Monitor %i, mode %3i: %4i x %4i @ %ihz\n", displayNum+1, i, modeList[i].width, modeList[i].height, modeList[i].displayHz );
		}
	}
}

/*
=============
R_TestImage_f

Display the given image centered on the screen.
testimage <number>
testimage <filename>
=============
*/
void R_TestImage_f( const idCmdArgs &args ) {
	int imageNum;

	if ( tr.testVideo ) {
		delete tr.testVideo;
		tr.testVideo = NULL;
	}
	tr.testImage = NULL;

	if ( args.Argc() != 2 ) {
		return;
	}

	if ( idStr::IsNumeric( args.Argv(1) ) ) {
		imageNum = atoi( args.Argv(1) );
		if ( imageNum >= 0 && imageNum < globalImages->images.Num() ) {
			tr.testImage = globalImages->images[imageNum];
		}
	} else {
		tr.testImage = globalImages->ImageFromFile( args.Argv( 1 ), TF_DEFAULT, TR_REPEAT, TD_DEFAULT );
	}
}

/*
=============
R_TestVideo_f

Plays the cinematic file in a testImage
=============
*/
void R_TestVideo_f( const idCmdArgs &args ) {
	if ( tr.testVideo ) {
		delete tr.testVideo;
		tr.testVideo = NULL;
	}
	tr.testImage = NULL;

	if ( args.Argc() < 2 ) {
		return;
	}

	tr.testImage = globalImages->ImageFromFile( "_scratch", TF_DEFAULT, TR_REPEAT, TD_DEFAULT );
	tr.testVideo = idCinematic::Alloc();
	tr.testVideo->InitFromFile( args.Argv( 1 ), true );

	// starts it now; the backend asks for frames with Sys_Milliseconds
	cinData_t	cin;
	cin = tr.testVideo->ImageForTime( Sys_Milliseconds() );
	if ( cin.imageY == NULL ) {
		delete tr.testVideo;
		tr.testVideo = NULL;
		tr.testImage = NULL;
		return;
	}

	common->Printf( "%i x %i images\n", cin.imageWidth, cin.imageHeight );

	int	len = tr.testVideo->AnimationLength();
	common->Printf( "%5.1f seconds of video\n", len * 0.001 );

	// try to play the matching wav file
	idStr	wavString = args.Argv( ( args.Argc() == 2 ) ? 1 : 2 );
	wavString.StripFileExtension();
	wavString = wavString + ".wav";
	common->SW()->PlayShaderDirectly( wavString.c_str() );
}

/*
=============
R_ExportVideo_f

Writes every frame of a cinematic to <file without extension>/frameNNNNN.tga in fs_savepath
=============
*/
static void R_ExportVideo_f( const idCmdArgs &args ) {
	if ( args.Argc() < 2 ) {
		common->Printf( "USAGE: exportVideo <video/file.bik>\n" );
		return;
	}
	std::unique_ptr<idCinematic> cinematic( idCinematic::Alloc() );
	if ( !cinematic->InitFromFile( args.Argv( 1 ), false ) ) {
		common->Printf( "can't play %s\n", args.Argv( 1 ) );
		return;
	}
	cinematic->ExportToTGA( false );
}

static int R_QsortSurfaceAreas( const void *a, const void *b ) {
	const idMaterial	*ea, *eb;
	int	ac, bc;

	ea = *(idMaterial **)a;
	if ( !ea->EverReferenced() ) {
		ac = 0;
	} else {
		ac = ea->GetSurfaceArea();
	}
	eb = *(idMaterial **)b;
	if ( !eb->EverReferenced() ) {
		bc = 0;
	} else {
		bc = eb->GetSurfaceArea();
	}

	if ( ac < bc ) {
		return -1;
	}
	if ( ac > bc ) {
		return 1;
	}

	return idStr::Icmp( ea->GetName(), eb->GetName() );
}


/*
===================
R_ReportSurfaceAreas_f

Prints a list of the materials sorted by surface area
===================
*/
#pragma warning( disable: 6385 ) // This is simply to get pass a false defect for /analyze -- if you can figure out a better way, please let Shawn know...
void R_ReportSurfaceAreas_f( const idCmdArgs &args ) {
	unsigned int		i;
	idMaterial	**list;

	const unsigned int count = declManager->GetNumDecls( DECL_MATERIAL );
	if ( count == 0 ) {
		return;
	}

	list = (idMaterial **)_alloca( count * sizeof( *list ) );

	for ( i = 0 ; i < count ; i++ ) {
		list[i] = (idMaterial *)declManager->DeclByIndex( DECL_MATERIAL, i, false );
	}

	qsort( list, count, sizeof( list[0] ), R_QsortSurfaceAreas );

	// skip over ones with 0 area
	for ( i = 0 ; i < count ; i++ ) {
		if ( list[i]->GetSurfaceArea() > 0 ) {
			break;
		}
	}

	for ( ; i < count ; i++ ) {
		// report size in "editor blocks"
		int	blocks = list[i]->GetSurfaceArea() / 4096.0;
		common->Printf( "%7i %s\n", blocks, list[i]->GetName() );
	}
}
#pragma warning( default: 6385 )


/* 
============================================================================== 
 
						SCREEN SHOTS 
 
============================================================================== 
*/ 

/*
====================
R_ReadTiledPixels

NO LONGER SUPPORTED (FIXME: make standard case work)

Used to allow the rendering of an image larger than the actual window by
tiling it into window-sized chunks and rendering each chunk separately

If ref isn't specified, the full session UpdateScreen will be done.

Each tile is read from the output right after it was rendered, before it is presented.
====================
*/
void R_ReadTiledPixels( int width, int height, byte *buffer, renderView_t *ref = NULL ) {
	const int sysWidth = renderSystem->GetWidth();
	const int sysHeight = renderSystem->GetHeight();
	byte * temp = (byte *)R_StaticAlloc( sysWidth * sysHeight * 3 );

	// tiles disable the scissor, so we don't need to adjust all those rects; a single window-sized
	// render keeps it, or the scissored debug drawing would differ from the frame on screen
	const bool tiled = width > sysWidth || height > sysHeight;
	const bool useScissor = r_useScissor.GetBool();
	if ( tiled ) {
		r_useScissor.SetBool( false );
	}

	for ( int xo = 0 ; xo < width ; xo += sysWidth ) {
		for ( int yo = 0 ; yo < height ; yo += sysHeight ) {
			if ( ref ) {
				// discard anything currently on the list
				tr.SwapCommandBuffers( NULL, NULL, NULL, NULL );

				// build commands to render the scene
				tr.primaryWorld->RenderScene( ref );

				// finish off these commands
				const emptyCommand_t * cmd = tr.SwapCommandBuffers( NULL, NULL, NULL, NULL );

				// issue the commands to the GPU
				tr.RenderCommandBuffers( cmd );
			} else {
				const bool captureToImage = false;
				common->UpdateScreen( captureToImage );
			}

			int w = sysWidth;
			if ( xo + w > width ) {
				w = width - xo;
			}
			int h = sysHeight;
			if ( yo + h > height ) {
				h = height - yo;
			}

			renderBackend->ReadOutputPixels( 0, 0, w, h, temp );

			const int row = w * 3;		// tightly packed rows, bottom row first

			for ( int y = 0 ; y < h ; y++ ) {
				memcpy( buffer + ( ( yo + y )* width + xo ) * 3,
					temp + y * row, w * 3 );
			}
		}
	}

	r_useScissor.SetBool( useScissor );

	R_StaticFree( temp );
}


/*
================== 
TakeScreenshot

Move to tr_imagefiles.c...

Downsample is the number of steps to mipmap the image before saving it
If ref == NULL, common->UpdateScreen will be used
================== 
*/  
void idRenderSystemLocal::TakeScreenshot( int width, int height, const char *fileName, int blends, renderView_t *ref ) {
	byte		*buffer;
	int			i, j, c, temp;

	takingScreenshot = true;

	const int pix = width * height;
	const int bufferSize = pix * 3 + 18;

	buffer = (byte *)R_StaticAlloc( bufferSize );
	memset( buffer, 0, bufferSize );

	if ( blends <= 1 ) {
		R_ReadTiledPixels( width, height, buffer + 18, ref );
	} else {
		unsigned short *shortBuffer = (unsigned short *)R_StaticAlloc(pix*2*3);
		memset (shortBuffer, 0, pix*2*3);

		// enable anti-aliasing jitter
		r_jitter.SetBool( true );

		for ( i = 0 ; i < blends ; i++ ) {
			R_ReadTiledPixels( width, height, buffer + 18, ref );

			for ( j = 0 ; j < pix*3 ; j++ ) {
				shortBuffer[j] += buffer[18+j];
			}
		}

		// divide back to bytes
		for ( i = 0 ; i < pix*3 ; i++ ) {
			buffer[18+i] = shortBuffer[i] / blends;
		}

		R_StaticFree( shortBuffer );
		r_jitter.SetBool( false );
	}

	// fill in the header (this is vertically flipped, which ReadOutputPixels emits)
	buffer[2] = 2;		// uncompressed type
	buffer[12] = width & 255;
	buffer[13] = width >> 8;
	buffer[14] = height & 255;
	buffer[15] = height >> 8;
	buffer[16] = 24;	// pixel size

	// swap rgb to bgr
	c = 18 + width * height * 3;
	for (i=18 ; i<c ; i+=3) {
		temp = buffer[i];
		buffer[i] = buffer[i+2];
		buffer[i+2] = temp;
	}

	fileSystem->WriteFile( fileName, buffer, c );

	R_StaticFree( buffer );

	takingScreenshot = false;
}

/* 
================== 
R_ScreenshotFilename

Returns a filename with digits appended
if we have saved a previous screenshot, don't scan
from the beginning, because recording demo avis can involve
thousands of shots
================== 
*/  
void R_ScreenshotFilename( int &lastNumber, const char *base, idStr &fileName ) {
	int	a,b,c,d, e;

	bool restrict = cvarSystem->GetCVarBool( "fs_restrict" );
	cvarSystem->SetCVarBool( "fs_restrict", false );

	lastNumber++;
	if ( lastNumber > 99999 ) {
		lastNumber = 99999;
	}
	for ( ; lastNumber < 99999 ; lastNumber++ ) {
		int	frac = lastNumber;

		a = frac / 10000;
		frac -= a*10000;
		b = frac / 1000;
		frac -= b*1000;
		c = frac / 100;
		frac -= c*100;
		d = frac / 10;
		frac -= d*10;
		e = frac;

		sprintf( fileName, "%s%i%i%i%i%i.tga", base, a, b, c, d, e );
		if ( lastNumber == 99999 ) {
			break;
		}
		int len = fileSystem->ReadFile( fileName, NULL, NULL );
		if ( len <= 0 ) {
			break;
		}
		// check again...
	}
	cvarSystem->SetCVarBool( "fs_restrict", restrict );
}

/*
================== 
R_BlendedScreenShot

screenshot
screenshot [filename]
screenshot [width] [height]
screenshot [width] [height] [samples]
================== 
*/ 
#define	MAX_BLENDS	256	// to keep the accumulation in shorts
void R_ScreenShot_f( const idCmdArgs &args ) {
	static int lastNumber = 0;
	idStr checkname;

	int width = renderSystem->GetWidth();
	int height = renderSystem->GetHeight();
	int	blends = 0;

	switch ( args.Argc() ) {
	case 1:
		width = renderSystem->GetWidth();
		height = renderSystem->GetHeight();
		blends = 1;
		R_ScreenshotFilename( lastNumber, "screenshots/shot", checkname );
		break;
	case 2:
		width = renderSystem->GetWidth();
		height = renderSystem->GetHeight();
		blends = 1;
		checkname = args.Argv( 1 );
		break;
	case 3:
		width = atoi( args.Argv( 1 ) );
		height = atoi( args.Argv( 2 ) );
		blends = 1;
		R_ScreenshotFilename( lastNumber, "screenshots/shot", checkname );
		break;
	case 4:
		width = atoi( args.Argv( 1 ) );
		height = atoi( args.Argv( 2 ) );
		blends = atoi( args.Argv( 3 ) );
		if ( blends < 1 ) {
			blends = 1;
		}
		if ( blends > MAX_BLENDS ) {
			blends = MAX_BLENDS;
		}
		R_ScreenshotFilename( lastNumber, "screenshots/shot", checkname );
		break;
	default:
		common->Printf( "usage: screenshot\n       screenshot <filename>\n       screenshot <width> <height>\n       screenshot <width> <height> <blends>\n" );
		return;
	}

	// put the console away
	console->Close();

	tr.TakeScreenshot( width, height, checkname, blends, NULL );

	common->Printf( "Wrote %s\n", checkname.c_str() );
}

//============================================================================

static idMat3		cubeAxis[6];


/*
==================
R_SampleCubeMap
==================
*/
void R_SampleCubeMap( const idVec3 &dir, int size, byte *buffers[6], byte result[4] ) {
	float	adir[3];
	int		axis, x, y;

	adir[0] = fabs(dir[0]);
	adir[1] = fabs(dir[1]);
	adir[2] = fabs(dir[2]);

	if ( dir[0] >= adir[1] && dir[0] >= adir[2] ) {
		axis = 0;
	} else if ( -dir[0] >= adir[1] && -dir[0] >= adir[2] ) {
		axis = 1;
	} else if ( dir[1] >= adir[0] && dir[1] >= adir[2] ) {
		axis = 2;
	} else if ( -dir[1] >= adir[0] && -dir[1] >= adir[2] ) {
		axis = 3;
	} else if ( dir[2] >= adir[1] && dir[2] >= adir[2] ) {
		axis = 4;
	} else {
		axis = 5;
	}

	float	fx = (dir * cubeAxis[axis][1]) / (dir * cubeAxis[axis][0]);
	float	fy = (dir * cubeAxis[axis][2]) / (dir * cubeAxis[axis][0]);

	fx = -fx;
	fy = -fy;
	x = size * 0.5 * (fx + 1);
	y = size * 0.5 * (fy + 1);
	if ( x < 0 ) {
		x = 0;
	} else if ( x >= size ) {
		x = size-1;
	}
	if ( y < 0 ) {
		y = 0;
	} else if ( y >= size ) {
		y = size-1;
	}

	result[0] = buffers[axis][(y*size+x)*4+0];
	result[1] = buffers[axis][(y*size+x)*4+1];
	result[2] = buffers[axis][(y*size+x)*4+2];
	result[3] = buffers[axis][(y*size+x)*4+3];
}

/* 
================== 
R_MakeAmbientMap_f

R_MakeAmbientMap_f <basename> [size]

Saves out env/<basename>_amb_ft.tga, etc
================== 
*/  
void R_MakeAmbientMap_f( const idCmdArgs &args ) {
	idStr fullname;
	const char	*baseName;
	int			i;
	renderView_t	ref;
	viewDef_t	primary;
	int			downSample;
	const char *extensions[6] =  { "_px.tga", "_nx.tga", "_py.tga", "_ny.tga", 
		"_pz.tga", "_nz.tga" };
	int			outSize;
	byte		*buffers[6];
	int			width = 0, height = 0;

	if ( args.Argc() != 2 && args.Argc() != 3 ) {
		common->Printf( "USAGE: ambientshot <basename> [size]\n" );
		return;
	}
	baseName = args.Argv( 1 );

	downSample = 0;
	if ( args.Argc() == 3 ) {
		outSize = atoi( args.Argv( 2 ) );
	} else {
		outSize = 32;
	}

	memset( &cubeAxis, 0, sizeof( cubeAxis ) );
	cubeAxis[0][0][0] = 1;
	cubeAxis[0][1][2] = 1;
	cubeAxis[0][2][1] = 1;

	cubeAxis[1][0][0] = -1;
	cubeAxis[1][1][2] = -1;
	cubeAxis[1][2][1] = 1;

	cubeAxis[2][0][1] = 1;
	cubeAxis[2][1][0] = -1;
	cubeAxis[2][2][2] = -1;

	cubeAxis[3][0][1] = -1;
	cubeAxis[3][1][0] = -1;
	cubeAxis[3][2][2] = 1;

	cubeAxis[4][0][2] = 1;
	cubeAxis[4][1][0] = -1;
	cubeAxis[4][2][1] = 1;

	cubeAxis[5][0][2] = -1;
	cubeAxis[5][1][0] = 1;
	cubeAxis[5][2][1] = 1;

	// read all of the images
	for ( i = 0 ; i < 6 ; i++ ) {
		sprintf( fullname, "env/%s%s", baseName, extensions[i] );
		common->Printf( "loading %s\n", fullname.c_str() );
		const bool captureToImage = false;
		common->UpdateScreen( captureToImage );
		R_LoadImage( fullname, &buffers[i], &width, &height, NULL, true );
		if ( !buffers[i] ) {
			common->Printf( "failed.\n" );
			for ( i-- ; i >= 0 ; i-- ) {
				Mem_Free( buffers[i] );
			}
			return;
		}
	}

	// resample with hemispherical blending
	int	samples = 1000;

	byte	*outBuffer = (byte *)_alloca( outSize * outSize * 4 );

	for ( int map = 0 ; map < 2 ; map++ ) {
		for ( i = 0 ; i < 6 ; i++ ) {
			for ( int x = 0 ; x < outSize ; x++ ) {
				for ( int y = 0 ; y < outSize ; y++ ) {
					idVec3	dir;
					float	total[3];

					dir = cubeAxis[i][0] + -( -1 + 2.0*x/(outSize-1) ) * cubeAxis[i][1] + -( -1 + 2.0*y/(outSize-1) ) * cubeAxis[i][2];
					dir.Normalize();
					total[0] = total[1] = total[2] = 0;
	//samples = 1;
					float	limit = map ? 0.95 : 0.25;		// small for specular, almost hemisphere for ambient

					for ( int s = 0 ; s < samples ; s++ ) {
						// pick a random direction vector that is inside the unit sphere but not behind dir,
						// which is a robust way to evenly sample a hemisphere
						idVec3	test;
						while( 1 ) {
							for ( int j = 0 ; j < 3 ; j++ ) {
								test[j] = -1 + 2 * (rand()&0x7fff)/(float)0x7fff;
							}
							if ( test.Length() > 1.0 ) {
								continue;
							}
							test.Normalize();
							if ( test * dir > limit ) {	// don't do a complete hemisphere
								break;
							}
						}
						byte	result[4];
	//test = dir;
						R_SampleCubeMap( test, width, buffers, result );
						total[0] += result[0];
						total[1] += result[1];
						total[2] += result[2];
					}
					outBuffer[(y*outSize+x)*4+0] = total[0] / samples;
					outBuffer[(y*outSize+x)*4+1] = total[1] / samples;
					outBuffer[(y*outSize+x)*4+2] = total[2] / samples;
					outBuffer[(y*outSize+x)*4+3] = 255;
				}
			}

			if ( map == 0 ) {
				sprintf( fullname, "env/%s_amb%s", baseName, extensions[i] );
			} else {
				sprintf( fullname, "env/%s_spec%s", baseName, extensions[i] );
			}
			common->Printf( "writing %s\n", fullname.c_str() );
			const bool captureToImage = false;
			common->UpdateScreen( captureToImage );
			R_WriteTGA( fullname, outBuffer, outSize, outSize );
		}
	}

	for ( i = 0 ; i < 6 ; i++ ) {
		if ( buffers[i] ) {
			Mem_Free( buffers[i] );
		}
	}
} 

//============================================================================


/*
===============
R_SetColorMappings
===============
*/
void R_SetColorMappings() {
	float b = r_brightness.GetFloat();
	float invg = 1.0f / r_gamma.GetFloat();

	float j = 0.0f;
	for ( int i = 0; i < 256; i++, j += b ) {
		int inf = idMath::Ftoi( 0xffff * pow( j / 255.0f, invg ) + 0.5f );
		tr.gammaTable[i] = idMath::ClampInt( 0, 0xFFFF, inf );
	}

	renderBackend->SetGammaRamp( tr.gammaTable, tr.gammaTable, tr.gammaTable );
}

/*
================
GfxInfo_f
================
*/
void GfxInfo_f( const idCmdArgs &args ) {
	common->Printf( "CPU: %s\n", Sys_GetProcessorString() );

	renderBackend->PrintInfo();
}

/*
=================
R_VidRestart_f
=================
*/
void R_VidRestart_f( const idCmdArgs &args ) {
	// if the backend isn't started, do nothing
	if ( !R_IsInitialized() ) {
		return;
	}

	// set the mode without re-initializing the context
	R_SetNewMode( false );
}

/*
=================
R_InitMaterials
=================
*/
void R_InitMaterials() {
	tr.defaultMaterial = declManager->FindMaterial( "_default", false );
	if ( !tr.defaultMaterial ) {
		common->FatalError( "_default material not found" );
	}
	tr.defaultPointLight = declManager->FindMaterial( "lights/defaultPointLight" );
	tr.defaultProjectedLight = declManager->FindMaterial( "lights/defaultProjectedLight" );
	tr.whiteMaterial = declManager->FindMaterial( "_white" );
	tr.charSetMaterial = declManager->FindMaterial( "textures/bigchars" );
}


/*
=================
R_SizeUp_f

Keybinding command
=================
*/
static void R_SizeUp_f( const idCmdArgs &args ) {
	if ( r_screenFraction.GetInteger() + 10 > 100 ) {
		r_screenFraction.SetInteger( 100 );
	} else {
		r_screenFraction.SetInteger( r_screenFraction.GetInteger() + 10 );
	}
}


/*
=================
R_SizeDown_f

Keybinding command
=================
*/
static void R_SizeDown_f( const idCmdArgs &args ) {
	if ( r_screenFraction.GetInteger() - 10 < 10 ) {
		r_screenFraction.SetInteger( 10 );
	} else {
		r_screenFraction.SetInteger( r_screenFraction.GetInteger() - 10 );
	}
}


/*
===============
TouchGui_f

  this is called from the main thread
===============
*/
void R_TouchGui_f( const idCmdArgs &args ) {
	const char	*gui = args.Argv( 1 );

	if ( !gui[0] ) {
		common->Printf( "USAGE: touchGui <guiName>\n" );
		return;
	}

	common->Printf( "touchGui %s\n", gui );
	const bool captureToImage = false;
	common->UpdateScreen( captureToImage );
	uiManager->Touch( gui );
}

/*
=================
R_InitCvars
=================
*/
void R_InitCvars() {
	// update latched cvars here
}

/*
=================
R_InitCommands
=================
*/
void R_InitCommands() {
	cmdSystem->AddCommand( "sizeUp", R_SizeUp_f, CMD_FL_RENDERER, "makes the rendered view larger" );
	cmdSystem->AddCommand( "sizeDown", R_SizeDown_f, CMD_FL_RENDERER, "makes the rendered view smaller" );
	cmdSystem->AddCommand( "reloadGuis", R_ReloadGuis_f, CMD_FL_RENDERER, "reloads guis" );
	cmdSystem->AddCommand( "listGuis", R_ListGuis_f, CMD_FL_RENDERER, "lists guis" );
	cmdSystem->AddCommand( "touchGui", R_TouchGui_f, CMD_FL_RENDERER, "touches a gui" );
	cmdSystem->AddCommand( "screenshot", R_ScreenShot_f, CMD_FL_RENDERER, "takes a screenshot" );
	cmdSystem->AddCommand( "makeAmbientMap", R_MakeAmbientMap_f, CMD_FL_RENDERER|CMD_FL_CHEAT, "makes an ambient map" );
	cmdSystem->AddCommand( "gfxInfo", GfxInfo_f, CMD_FL_RENDERER, "show graphics info" );
	cmdSystem->AddCommand( "modulateLights", R_ModulateLights_f, CMD_FL_RENDERER | CMD_FL_CHEAT, "modifies shader parms on all lights" );
	cmdSystem->AddCommand( "testImage", R_TestImage_f, CMD_FL_RENDERER | CMD_FL_CHEAT, "displays the given image centered on screen", idCmdSystem::ArgCompletion_ImageName );
	cmdSystem->AddCommand( "testVideo", R_TestVideo_f, CMD_FL_RENDERER | CMD_FL_CHEAT, "displays the given cinematic", idCmdSystem::ArgCompletion_VideoName );
	cmdSystem->AddCommand( "exportVideo", R_ExportVideo_f, CMD_FL_RENDERER, "writes the frames of a cinematic as TGA files to fs_savepath", idCmdSystem::ArgCompletion_VideoName );
	cmdSystem->AddCommand( "reportSurfaceAreas", R_ReportSurfaceAreas_f, CMD_FL_RENDERER, "lists all used materials sorted by surface area" );
	cmdSystem->AddCommand( "showInteractionMemory", R_ShowInteractionMemory_f, CMD_FL_RENDERER, "shows memory used by interactions" );
	cmdSystem->AddCommand( "vid_restart", R_VidRestart_f, CMD_FL_RENDERER, "restarts renderSystem" );
	cmdSystem->AddCommand( "listRenderEntityDefs", R_ListRenderEntityDefs_f, CMD_FL_RENDERER, "lists the entity defs" );
	cmdSystem->AddCommand( "listRenderLightDefs", R_ListRenderLightDefs_f, CMD_FL_RENDERER, "lists the light defs" );
	cmdSystem->AddCommand( "listModes", R_ListModes_f, CMD_FL_RENDERER, "lists all video modes" );
	cmdSystem->AddCommand( "reloadSurface", R_ReloadSurface_f, CMD_FL_RENDERER, "reloads the decl and images for selected surface" );
}

/*
===============
idRenderSystemLocal::Clear
===============
*/
void idRenderSystemLocal::Clear() {
	registered = false;
	frameCount = 0;
	viewCount = 0;
	frameShaderTime = 0.0f;
	ambientLightVector.Zero();
	worlds.Clear();
	primaryWorld = NULL;
	memset( &primaryRenderView, 0, sizeof( primaryRenderView ) );
	primaryView = NULL;
	defaultMaterial = NULL;
	testImage = NULL;
	ambientCubeImage = NULL;
	viewDef = NULL;
	memset( &pc, 0, sizeof( pc ) );
	memset( &identitySpace, 0, sizeof( identitySpace ) );
	memset( renderCrops, 0, sizeof( renderCrops ) );
	currentRenderCrop = 0;
	currentColorNativeBytesOrder = 0xFFFFFFFF;
	currentGLState = 0;
	guiRecursionLevel = 0;
	guiModel = NULL;
	memset( gammaTable, 0, sizeof( gammaTable ) );
	takingScreenshot = false;

	if ( unitSquareTriangles != NULL ) {
		Mem_Free( unitSquareTriangles );
		unitSquareTriangles = NULL;
	}

	if ( zeroOneCubeTriangles != NULL ) {
		Mem_Free( zeroOneCubeTriangles );
		zeroOneCubeTriangles = NULL;
	}

	if ( testImageTriangles != NULL ) {
		Mem_Free( testImageTriangles );
		testImageTriangles = NULL;
	}

	frontEndJobList = NULL;
}

/*
=============
R_MakeFullScreenTris
=============
*/
static srfTriangles_t * R_MakeFullScreenTris() {
	// copy verts and indexes
	srfTriangles_t * tri = (srfTriangles_t *)Mem_ClearedAlloc( sizeof( *tri ), TAG_RENDER_TOOLS );

	tri->numIndexes = 6;
	tri->numVerts = 4;

	int indexSize = tri->numIndexes * sizeof( tri->indexes[0] );
	int allocatedIndexBytes = ALIGN( indexSize, 16 );
	tri->indexes = (triIndex_t *)Mem_Alloc( allocatedIndexBytes, TAG_RENDER_TOOLS );

	int vertexSize = tri->numVerts * sizeof( tri->verts[0] );
	int allocatedVertexBytes =  ALIGN( vertexSize, 16 );
	tri->verts = (idDrawVert *)Mem_ClearedAlloc( allocatedVertexBytes, TAG_RENDER_TOOLS );

	idDrawVert * verts = tri->verts;

	triIndex_t tempIndexes[6] = { 3, 0, 2, 2, 0, 1 };
	memcpy( tri->indexes, tempIndexes, indexSize );
	
	verts[0].xyz[0] = -1.0f;
	verts[0].xyz[1] = 1.0f;
	verts[0].SetTexCoord( 0.0f, 1.0f );

	verts[1].xyz[0] = 1.0f;
	verts[1].xyz[1] = 1.0f;
	verts[1].SetTexCoord( 1.0f, 1.0f );

	verts[2].xyz[0] = 1.0f;
	verts[2].xyz[1] = -1.0f;
	verts[2].SetTexCoord( 1.0f, 0.0f );

	verts[3].xyz[0] = -1.0f;
	verts[3].xyz[1] = -1.0f;
	verts[3].SetTexCoord( 0.0f, 0.0f );

	for ( int i = 0 ; i < 4 ; i++ ) {
		verts[i].SetColor( 0xffffffff );
	}


	return tri;
}

/*
=============
R_MakeZeroOneCubeTris
=============
*/
static srfTriangles_t * R_MakeZeroOneCubeTris() {
	srfTriangles_t * tri = (srfTriangles_t *)Mem_ClearedAlloc( sizeof( *tri ), TAG_RENDER_TOOLS );

	tri->numVerts = 8;
	tri->numIndexes = 36;

	const int indexSize = tri->numIndexes * sizeof( tri->indexes[0] );
	const int allocatedIndexBytes = ALIGN( indexSize, 16 );
	tri->indexes = (triIndex_t *)Mem_Alloc( allocatedIndexBytes, TAG_RENDER_TOOLS );

	const int vertexSize = tri->numVerts * sizeof( tri->verts[0] );
	const int allocatedVertexBytes =  ALIGN( vertexSize, 16 );
	tri->verts = (idDrawVert *)Mem_ClearedAlloc( allocatedVertexBytes, TAG_RENDER_TOOLS );

	idDrawVert * verts = tri->verts;

	const float low = 0.0f;
	const float high = 1.0f;

	idVec3 center( 0.0f );
	idVec3 mx(  low, 0.0f, 0.0f );
	idVec3 px( high, 0.0f, 0.0f );
	idVec3 my( 0.0f,  low, 0.0f );
	idVec3 py( 0.0f, high, 0.0f );
	idVec3 mz( 0.0f, 0.0f,  low );
	idVec3 pz( 0.0f, 0.0f, high );

	verts[0].xyz = center + mx + my + mz;
	verts[1].xyz = center + px + my + mz;
	verts[2].xyz = center + px + py + mz;
	verts[3].xyz = center + mx + py + mz;
	verts[4].xyz = center + mx + my + pz;
	verts[5].xyz = center + px + my + pz;
	verts[6].xyz = center + px + py + pz;
	verts[7].xyz = center + mx + py + pz;

	// bottom
	tri->indexes[ 0*3+0] = 2;
	tri->indexes[ 0*3+1] = 3;
	tri->indexes[ 0*3+2] = 0;
	tri->indexes[ 1*3+0] = 1;
	tri->indexes[ 1*3+1] = 2;
	tri->indexes[ 1*3+2] = 0;
	// back
	tri->indexes[ 2*3+0] = 5;
	tri->indexes[ 2*3+1] = 1;
	tri->indexes[ 2*3+2] = 0;
	tri->indexes[ 3*3+0] = 4;
	tri->indexes[ 3*3+1] = 5;
	tri->indexes[ 3*3+2] = 0;
	// left
	tri->indexes[ 4*3+0] = 7;
	tri->indexes[ 4*3+1] = 4;
	tri->indexes[ 4*3+2] = 0;
	tri->indexes[ 5*3+0] = 3;
	tri->indexes[ 5*3+1] = 7;
	tri->indexes[ 5*3+2] = 0;
	// right
	tri->indexes[ 6*3+0] = 1;
	tri->indexes[ 6*3+1] = 5;
	tri->indexes[ 6*3+2] = 6;
	tri->indexes[ 7*3+0] = 2;
	tri->indexes[ 7*3+1] = 1;
	tri->indexes[ 7*3+2] = 6;
	// front
	tri->indexes[ 8*3+0] = 3;
	tri->indexes[ 8*3+1] = 2;
	tri->indexes[ 8*3+2] = 6;
	tri->indexes[ 9*3+0] = 7;
	tri->indexes[ 9*3+1] = 3;
	tri->indexes[ 9*3+2] = 6;
	// top
	tri->indexes[10*3+0] = 4;
	tri->indexes[10*3+1] = 7;
	tri->indexes[10*3+2] = 6;
	tri->indexes[11*3+0] = 5;
	tri->indexes[11*3+1] = 4;
	tri->indexes[11*3+2] = 6;

	for ( int i = 0 ; i < 4 ; i++ ) {
		verts[i].SetColor( 0xffffffff );
	}

	return tri;
}

/*
================
R_MakeTestImageTriangles

Initializes the Test Image Triangles
================
*/
srfTriangles_t* R_MakeTestImageTriangles() {
	srfTriangles_t * tri = (srfTriangles_t *)Mem_ClearedAlloc( sizeof( *tri ), TAG_RENDER_TOOLS );

	tri->numIndexes = 6;
	tri->numVerts = 4;

	int indexSize = tri->numIndexes * sizeof( tri->indexes[0] );
	int allocatedIndexBytes = ALIGN( indexSize, 16 );
	tri->indexes = (triIndex_t *)Mem_Alloc( allocatedIndexBytes, TAG_RENDER_TOOLS );

	int vertexSize = tri->numVerts * sizeof( tri->verts[0] );
	int allocatedVertexBytes =  ALIGN( vertexSize, 16 );
	tri->verts = (idDrawVert *)Mem_ClearedAlloc( allocatedVertexBytes, TAG_RENDER_TOOLS );

	ALIGNTYPE16 triIndex_t tempIndexes[6] = { 3, 0, 2, 2, 0, 1 };
	memcpy( tri->indexes, tempIndexes, indexSize );

	idDrawVert* tempVerts = tri->verts;
	tempVerts[0].xyz[0] = 0.0f;
	tempVerts[0].xyz[1] = 0.0f;
	tempVerts[0].xyz[2] = 0;
	tempVerts[0].SetTexCoord( 0.0, 0.0f );

	tempVerts[1].xyz[0] = 1.0f;
	tempVerts[1].xyz[1] = 0.0f;
	tempVerts[1].xyz[2] = 0;
	tempVerts[1].SetTexCoord( 1.0f, 0.0f );

	tempVerts[2].xyz[0] = 1.0f;
	tempVerts[2].xyz[1] = 1.0f;
	tempVerts[2].xyz[2] = 0;
	tempVerts[2].SetTexCoord( 1.0f, 1.0f );

	tempVerts[3].xyz[0] = 0.0f;
	tempVerts[3].xyz[1] = 1.0f;
	tempVerts[3].xyz[2] = 0;
	tempVerts[3].SetTexCoord( 0.0f, 1.0f );

	for ( int i = 0; i < 4; i++ ) {
		tempVerts[i].SetColor( 0xFFFFFFFF );
	}
	return tri;
}

/*
===============
idRenderSystemLocal::Init
===============
*/
void idRenderSystemLocal::Init() {	

	common->Printf( "------- Initializing renderSystem --------\n" );

	// clear all our internal state
	viewCount = 1;		// so cleared structures never match viewCount
	// we used to memset tr, but now that it is a class, we can't, so
	// there may be other state we need to reset

	ambientLightVector[0] = 0.5f;
	ambientLightVector[1] = 0.5f - 0.385f;
	ambientLightVector[2] = 0.8925f;
	ambientLightVector[3] = 1.0f;

	memset( &backEnd, 0, sizeof( backEnd ) );

	R_InitCvars();

	R_InitCommands();

	guiModel = new (TAG_RENDER) idGuiModel;
	guiModel->Clear();
	tr_guiModel = guiModel;	// for DeviceContext fast path

	globalImages->Init();

	idCinematic::InitCinematic( );

	// build brightness translation tables
	R_SetColorMappings();

	R_InitMaterials();

	renderModelManager->Init();

	// set the identity space
	identitySpace.modelMatrix[0*4+0] = 1.0f;
	identitySpace.modelMatrix[1*4+1] = 1.0f;
	identitySpace.modelMatrix[2*4+2] = 1.0f;

	// make sure the tr.unitSquareTriangles data is current in the vertex / index cache
	if ( unitSquareTriangles == NULL ) {
		unitSquareTriangles = R_MakeFullScreenTris();
	}
	// make sure the tr.zeroOneCubeTriangles data is current in the vertex / index cache
	if ( zeroOneCubeTriangles == NULL ) {
		zeroOneCubeTriangles = R_MakeZeroOneCubeTris();
	}
	// make sure the tr.testImageTriangles data is current in the vertex / index cache
	if ( testImageTriangles == NULL )  {
		testImageTriangles = R_MakeTestImageTriangles();
	}

	frontEndJobList = parallelJobManager->AllocJobList( JOBLIST_RENDERER_FRONTEND, JOBLIST_PRIORITY_MEDIUM, 2048, 0, NULL );

	// make sure the command buffers are ready to accept the first screen update
	SwapCommandBuffers( NULL, NULL, NULL, NULL );

	common->Printf( "renderSystem initialized.\n" );
	common->Printf( "--------------------------------------\n" );
}

/*
===============
idRenderSystemLocal::Shutdown
===============
*/
void idRenderSystemLocal::Shutdown() {	
	common->Printf( "idRenderSystem::Shutdown()\n" );

	fonts.DeleteContents();

	if ( R_IsInitialized() ) {
		globalImages->PurgeAllImages();
	}

	renderModelManager->Shutdown();

	idCinematic::ShutdownCinematic( );

	globalImages->Shutdown();

	// free frame memory
	R_ShutdownFrameData();

	// free the vertex cache, which should have nothing allocated now
	vertexCache.Shutdown();

	RB_ShutdownDebugTools();

	delete guiModel;

	parallelJobManager->FreeJobList( frontEndJobList );

	Clear();

	ShutdownBackend();

	delete renderBackend;
	renderBackend = nullptr;
}

/*
========================
idRenderSystemLocal::ResetGuiModels
========================
*/
void idRenderSystemLocal::ResetGuiModels() {
	delete guiModel;
	guiModel = new (TAG_RENDER) idGuiModel;
	guiModel->Clear();
	guiModel->BeginFrame();
	tr_guiModel = guiModel;	// for DeviceContext fast path
}

/*
========================
idRenderSystemLocal::BeginLevelLoad
========================
*/
void idRenderSystemLocal::BeginLevelLoad() {
	globalImages->BeginLevelLoad();
	renderModelManager->BeginLevelLoad();

	// Re-Initialize the Default Materials if needed. 
	R_InitMaterials();
}

/*
========================
idRenderSystemLocal::LoadLevelImages
========================
*/
void idRenderSystemLocal::LoadLevelImages() {
	globalImages->LoadLevelImages( false );
}

/*
========================
idRenderSystemLocal::Preload
========================
*/
void idRenderSystemLocal::Preload( const idPreloadManifest &manifest, const char *mapName ) {
	globalImages->Preload( manifest, true );
	uiManager->Preload( mapName );
	renderModelManager->Preload( manifest );
}

/*
========================
idRenderSystemLocal::EndLevelLoad
========================
*/
void idRenderSystemLocal::EndLevelLoad() {
	renderModelManager->EndLevelLoad();
	globalImages->EndLevelLoad();
	if ( renderBackend != nullptr && renderBackend->IsOwnerThread() ) {
		renderBackend->EndLevelLoad();
	}
}

/*
========================
idRenderSystemLocal::BeginAutomaticBackgroundSwaps
========================
*/
void idRenderSystemLocal::BeginAutomaticBackgroundSwaps( autoRenderIconType_t icon ) {
}

/*
========================
idRenderSystemLocal::EndAutomaticBackgroundSwaps
========================
*/
void idRenderSystemLocal::EndAutomaticBackgroundSwaps() {
}

/*
========================
idRenderSystemLocal::AreAutomaticBackgroundSwapsRunning
========================
*/
bool idRenderSystemLocal::AreAutomaticBackgroundSwapsRunning( autoRenderIconType_t * icon ) const {
	return false;
}

/*
============
idRenderSystemLocal::RegisterFont
============
*/
idFont * idRenderSystemLocal::RegisterFont( const char * fontName ) {

	idStrStatic< MAX_OSPATH > baseFontName = fontName;
	baseFontName.Replace( "fonts/", "" );
	for ( int i = 0; i < fonts.Num(); i++ ) {
		if ( idStr::Icmp( fonts[i]->GetName(), baseFontName ) == 0 ) {
			fonts[i]->Touch();
			return fonts[i];
		}
	}
	idFont * newFont = new (TAG_FONT) idFont( baseFontName );
	fonts.Append( newFont );
	return newFont;
}

/*
========================
idRenderSystemLocal::ResetFonts
========================
*/
void idRenderSystemLocal::ResetFonts() {
	fonts.DeleteContents( true );
}
/*
========================
idRenderSystemLocal::InitBackend
========================
*/
void idRenderSystemLocal::InitBackend() {
	if ( !R_IsInitialized() ) {
		R_InitBackend();

		// Reloading images here causes the rendertargets to get deleted. Figure out how to handle this properly on 360
		globalImages->ReloadImages( true );
		
		renderBackend->CheckErrors();
	}
}

/*
========================
idRenderSystemLocal::ShutdownBackend
========================
*/
void idRenderSystemLocal::ShutdownBackend() {
	// the r_pix queries go away with the context
	PC_Shutdown();

	// free the context and close the window
	R_ShutdownFrameData();
	if ( renderBackend != nullptr ) {
		renderBackend->Shutdown();
	}
	Sys_DestroyWindow();
	r_initialized = false;
}

bool idRenderSystemLocal::HasHdrScene() const {
	return renderBackend != nullptr && renderBackend->Caps().hdrScene && r_hdr.GetBool();
}

/*
========================
idRenderSystemLocal::IsBackendRunning
========================
*/
bool idRenderSystemLocal::IsBackendRunning() const {
	return R_IsInitialized();
}

/*
========================
idRenderSystemLocal::IsFullScreen
========================
*/
bool idRenderSystemLocal::IsFullScreen() const {
	return screenConfig.isFullscreen != 0;
}

/*
========================
idRenderSystemLocal::GetDisplayRefreshRate
========================
*/
int idRenderSystemLocal::GetDisplayRefreshRate() const {
	return screenConfig.displayFrequency;
}

/*
========================
idRenderSystemLocal::GetWidth
========================
*/
int idRenderSystemLocal::GetWidth() const {
	if ( screenConfig.stereo3Dmode == STEREO3D_SIDE_BY_SIDE || screenConfig.stereo3Dmode == STEREO3D_SIDE_BY_SIDE_COMPRESSED ) {
		return screenConfig.nativeScreenWidth >> 1;
	}
	return screenConfig.nativeScreenWidth;
}

/*
========================
idRenderSystemLocal::GetHeight
========================
*/
int idRenderSystemLocal::GetHeight() const {
	if ( screenConfig.stereo3Dmode == STEREO3D_HDMI_720 ) {
		return 720;
	}
	extern idCVar stereoRender_warp;
	if ( screenConfig.stereo3Dmode == STEREO3D_SIDE_BY_SIDE && stereoRender_warp.GetBool() ) {
		// for the Rift, render a square aspect view that will be symetric for the optics
		return screenConfig.nativeScreenWidth >> 1;
	}
	if ( screenConfig.stereo3Dmode == STEREO3D_INTERLACED || screenConfig.stereo3Dmode == STEREO3D_TOP_AND_BOTTOM_COMPRESSED ) {
		return screenConfig.nativeScreenHeight >> 1;
	}
	return screenConfig.nativeScreenHeight;
}

/*
========================
idRenderSystemLocal::GetStereo3DMode
========================
*/
stereo3DMode_t idRenderSystemLocal::GetStereo3DMode() const {
	return screenConfig.stereo3Dmode;
}

/*
========================
idRenderSystemLocal::IsStereoScopicRenderingSupported
========================
*/
bool idRenderSystemLocal::IsStereoScopicRenderingSupported() const {
	return true;
}

/*
========================
idRenderSystemLocal::GetStereoScopicRenderingMode
========================
*/
stereo3DMode_t idRenderSystemLocal::GetStereoScopicRenderingMode() const {
	// an old config's 5, the OpenGL quad buffer, or any unknown value is mono
	const int mode = stereoRender_enable.GetInteger();
	if ( !IsStereoScopicRenderingSupported() || mode <= STEREO3D_OFF || ( mode > STEREO3D_INTERLACED && mode != STEREO3D_HDMI_720 ) ) {
		return STEREO3D_OFF;
	}
	return static_cast<stereo3DMode_t>( mode );
}

/*
========================
idRenderSystemLocal::IsStereoScopicRenderingSupported
========================
*/
void idRenderSystemLocal::EnableStereoScopicRendering( const stereo3DMode_t mode ) const {
	stereoRender_enable.SetInteger( mode );
}

/*
========================
idRenderSystemLocal::GetPixelAspect
========================
*/
float idRenderSystemLocal::GetPixelAspect() const {
	switch( screenConfig.stereo3Dmode ) {
	case STEREO3D_SIDE_BY_SIDE_COMPRESSED:
		return screenConfig.pixelAspect * 2.0f;
	case STEREO3D_TOP_AND_BOTTOM_COMPRESSED:
	case STEREO3D_INTERLACED:
		return screenConfig.pixelAspect * 0.5f;
	default:
		return screenConfig.pixelAspect;
	}
}

/*
========================
idRenderSystemLocal::GetPhysicalScreenWidthInCentimeters

This is used to calculate stereoscopic screen offset for a given interocular distance.
========================
*/
idCVar	r_forceScreenWidthCentimeters( "r_forceScreenWidthCentimeters", "0", CVAR_RENDERER | CVAR_ARCHIVE, "Override screen width returned by hardware" );
float idRenderSystemLocal::GetPhysicalScreenWidthInCentimeters() const {
	if ( r_forceScreenWidthCentimeters.GetFloat() > 0 ) {
		return r_forceScreenWidthCentimeters.GetFloat();
	}
	return screenConfig.physicalScreenWidthInCentimeters;
}
