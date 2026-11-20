// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#include "../include/bindless.hlsli"

// Depth → an R32 texture. Target row r is scene row y + r, as glCopyTexImage2D lays the rows of its box out.
// The value is the one GL's GL_DEPTH_COMPONENT copy keeps: the 24-bit depth as a float, which shaders read in .x.
cbuffer DepthCopyParms : register( b1 ) {
	int4 origin;		// x, y of the box in the scene
};

#if MULTISAMPLED
TEXTURE2D( Texture2DMS<float>, depth, 0 );
#else
TEXTURE2D( Texture2D<float>, depth, 0 );
#endif

float PSMain( float4 position : SV_Position ) : SV_Target {
	int2 pixel = int2( position.xy ) + origin.xy;
#if MULTISAMPLED
	return depth.Load( pixel, 0 );
#else
	return depth.Load( int3( pixel, 0 ) );
#endif
}
