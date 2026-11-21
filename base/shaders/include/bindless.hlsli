// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma once

/*
	Textures and samplers are bindless (SM 6.6): a draw sets the 26 root constants of b3, and a stage takes
	its views out of the shader-visible heaps by those indices. GL's texture unit n is texture2D<n> or
	textureCube<n> with sampler<n>; the backend fills the members of the kind the program samples, and the
	reflection of the members a stage reads gives the program's samplerMask and cubeMask.

	  TEXTURE2D( Texture2D, bumpMap, 1 );		GL's unit 1 as a 2D texture
	  TEXTURECUBE( TextureCube, skyMap, 0 );	unit 0 as a cube
	  SAMPLER( bumpSampler, 1 );				the sampler of unit 1
*/

cbuffer DrawResources : register( b3 ) {
	uint texture2D0, texture2D1, texture2D2, texture2D3, texture2D4, texture2D5, texture2D6, texture2D7;
	uint textureCube0, textureCube1, textureCube2, textureCube3, textureCube4, textureCube5, textureCube6, textureCube7;
	uint sampler0, sampler1, sampler2, sampler3, sampler4, sampler5, sampler6, sampler7;
	uint colorFlags;		// include/colorspace.hlsli
	float preExposure;
};

#define TEXTURE2D( type, name, slot )		static const type name = ResourceDescriptorHeap[ texture2D##slot ]
#define TEXTURECUBE( type, name, slot )		static const type name = ResourceDescriptorHeap[ textureCube##slot ]
#define SAMPLER( name, slot )				static const SamplerState name = SamplerDescriptorHeap[ sampler##slot ]
