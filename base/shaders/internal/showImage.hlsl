// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#include "../include/bindless.hlsli"

// r_d3d12ShowImage: a 2D texture, or with CUBE=1 the six faces of a cube laid out as a cross. The panel's top row
// is v = 0, as GL draws an image; the target is in GL row order (row 0 at the bottom). Minified panels read the
// mip level of their scale, magnified ones the nearest texel of level 0.
cbuffer ShowParms : register( b1 ) {
	float4 panel;		// x, y, width, height in target pixels
	float4 options;		// x: 1 = alpha as grey, y: mip level, 0 = magnified
};

SAMPLER( linearSampler, 0 );
SAMPLER( pointSampler, 1 );
#if CUBE
TEXTURECUBE( TextureCube<float4>, image, 1 );
#else
TEXTURE2D( Texture2D<float4>, image, 0 );
#endif

float2 PanelUv( float4 position ) {
	return float2( ( position.x - panel.x ) / panel.z, 1.0 - ( position.y - panel.y ) / panel.w );
}

float4 Shown( float4 texel ) {
	return ( options.x != 0.0 ) ? float4( texel.aaa, 1.0 ) : float4( texel.rgb, 1.0 );
}

#if CUBE
// GL's cube face orientation, inverted: a face's (s, t) back to the direction that selects it.
float4 PSMain( float4 position : SV_Position ) : SV_Target {
	float2 cells = PanelUv( position ) * float2( 4.0, 3.0 );
	int2 cell = int2( cells );
	float2 st = frac( cells ) * 2.0 - 1.0;
	float3 direction;
	if ( cell.y == 0 && cell.x == 1 ) {
		direction = float3( st.x, 1.0, st.y );			// +Y
	} else if ( cell.y == 2 && cell.x == 1 ) {
		direction = float3( st.x, -1.0, -st.y );		// -Y
	} else if ( cell.y == 1 && cell.x == 0 ) {
		direction = float3( -1.0, -st.y, st.x );		// -X
	} else if ( cell.y == 1 && cell.x == 1 ) {
		direction = float3( st.x, -st.y, 1.0 );			// +Z
	} else if ( cell.y == 1 && cell.x == 2 ) {
		direction = float3( 1.0, -st.y, -st.x );		// +X
	} else if ( cell.y == 1 && cell.x == 3 ) {
		direction = float3( -st.x, -st.y, -1.0 );		// -Z
	} else {
		discard;
		direction = float3( 0.0, 0.0, 1.0 );
	}
	return Shown( ( options.y > 0.0 ) ? image.SampleLevel( linearSampler, direction, options.y ) : image.SampleLevel( pointSampler, direction, 0.0 ) );
}
#else
float4 PSMain( float4 position : SV_Position ) : SV_Target {
	float2 uv = PanelUv( position );
	return Shown( ( options.y > 0.0 ) ? image.SampleLevel( linearSampler, uv, options.y ) : image.SampleLevel( pointSampler, uv, 0.0 ) );
}
#endif
