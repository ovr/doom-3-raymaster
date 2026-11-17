// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
float4 idGLClipToD3D( float4 p ) { return float4( p.x, -p.y, ( p.z + p.w ) * 0.5, p.w ); }
float4 tex2D( Texture2D t, SamplerState s, float2 uv ) { return t.Sample( s, uv ); }
float4 tex2Dproj( Texture2D t, SamplerState s, float3 uvw ) { return t.Sample( s, uvw.xy / uvw.z ); }
float4 tex2Dproj( Texture2D t, SamplerState s, float4 uvw ) { return t.Sample( s, uvw.xy / uvw.w ); }
float4 tex2Dlod( Texture2D t, SamplerState s, float4 uv ) { return t.SampleLevel( s, uv.xy, uv.w ); }
float4 tex2Dbias( Texture2D t, SamplerState s, float4 uv ) { return t.SampleBias( s, uv.xy, uv.w ); }
float4 texCUBE( TextureCube t, SamplerState s, float3 dir ) { return t.Sample( s, dir ); }
cbuffer idVertexParms : register( b0 ) {
	float4 rpMVPmatrixX;
	float4 rpMVPmatrixY;
	float4 rpMVPmatrixZ;
	float4 rpMVPmatrixW;
	float4 rpTextureMatrixS;
	float4 rpTextureMatrixT;
	float4 rpTexGen0S;
	float4 rpTexGen0T;
	float4 rpTexGen0Enabled;
};
static float dot4 (float4 a , float4 b )  {return dot ( a , b ) ; }
 
static float dot4 (float2 a , float4 b )  {return dot ( float4 ( a , 0 , 1 ) , b ) ; }
 
struct VS_IN {
	float4 position : POSITION;
	float2 texcoord : TEXCOORD0;
};
struct VS_OUT {
	float4 position;
	float2 texcoord0;
};
void idMain (VS_IN vertex , inout VS_OUT result )  {
result . position . x = dot4 ( vertex . position , rpMVPmatrixX ) ; 
result . position . y = dot4 ( vertex . position , rpMVPmatrixY ) ; 
result . position . z = dot4 ( vertex . position , rpMVPmatrixZ ) ; 
result . position . w = dot4 ( vertex . position , rpMVPmatrixW ) ; if ( rpTexGen0Enabled . x > 0.0 ) { 
result . texcoord0 . x = dot4 ( vertex . position , rpTexGen0S ) ; 
result . texcoord0 . y = dot4 ( vertex . position , rpTexGen0T ) ; 
} else { 
result . texcoord0 . x = dot4 ( vertex . texcoord . xy , rpTextureMatrixS ) ; 
result . texcoord0 . y = dot4 ( vertex . texcoord . xy , rpTextureMatrixT ) ; 
} 
}
struct idVaryings {
	float4 position : SV_Position;
	float4 t0 : TEXCOORD0;
};
idVaryings vsMain( VS_IN vertex ) {
	VS_OUT result = (VS_OUT)0;
	idMain( vertex, result );
	precise float4 clip = idGLClipToD3D( result.position );
	idVaryings o;
	o.position = clip;
	o.t0 = float4( result.texcoord0, 0.0, 0.0 );
	return o;
}
